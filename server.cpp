// remclaude server: wraps the `claude` CLI as a subprocess, exposes named
// resumable sessions over a newline-delimited-JSON TCP protocol.
//
// Wire protocol (NDJSON, one JSON object per line, both directions):
//   -> {"token":"...","cmd":"list"}
//   <- {"ok":true,"sessions":[{"name":...,"id":...,"created":...,"last_used":...}]}
//   -> {"token":"...","cmd":"create","name":"..."}
//   <- {"ok":true,"name":"...","id":"..."}
//   -> {"token":"...","cmd":"delete","name":"..."}
//   <- {"ok":true}
//   -> {"token":"...","cmd":"prompt","name":"...","text":"..."}
//   <- {"ok":true,"result":"...","is_error":false,"cost_usd":0.01}
//   <- {"ok":false,"error":"..."}   (any command, on failure)
//
// Usage: server <config.json>
// See config.example.json for all fields.
//
// Run this from (or point project_workdir at) the directory whose Claude
// Code project bucket you want sessions to live in (transcripts land under
// ~/.claude/projects/<encoded-cwd>/<session-id>.jsonl).

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string g_project_dir;

struct Config {
    std::string bind_ip;
    int port = 0;
    std::string token;
    std::string project_workdir;  // empty = keep the server's own cwd
    std::string registry_path = "remclaude_sessions.json";
    std::string log_path = "remclaude.log";
    int max_connections = 8;
    int read_timeout_sec = 300;       // 0 disables the timeout
    size_t max_line_bytes = 1 << 20;  // 1 MiB
    int session_max_age_days = 0;     // 0 disables age-based eviction
    int max_sessions = 0;             // 0 disables the cap
};

Config g_cfg;

std::string now_iso() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return buf;
}

// Parses a "YYYY-MM-DDTHH:MM:SSZ" stamp (as produced by now_iso) into epoch
// seconds, for age comparisons. Returns -1 on failure.
long long parse_iso_to_epoch(const std::string& s) {
    std::tm tm{};
    if (strptime(s.c_str(), "%Y-%m-%dT%H:%M:%SZ", &tm) == nullptr) return -1;
    return static_cast<long long>(timegm(&tm));
}

std::string gen_uuid() {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, 15);
    static const char* hex = "0123456789abcdef";
    std::string s(36, '-');
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) continue;
        int v = dist(rng);
        if (i == 14) v = 4;                    // UUID version 4
        else if (i == 19) v = (v & 0x3) | 0x8;  // RFC 4122 variant
        s[i] = hex[v];
    }
    return s;
}

fs::path transcript_path(const std::string& id) {
    return fs::path(g_project_dir) / (id + ".jsonl");
}

// Appends structured JSON-lines audit entries. Deliberately never logs
// prompt/response *content* (only lengths/metadata) since that's sensitive
// by nature.
class Logger {
public:
    explicit Logger(std::string path) : path_(std::move(path)) {}

    void log(json entry) {
        entry["ts"] = now_iso();
        std::lock_guard<std::mutex> lk(mtx_);
        std::ofstream f(path_, std::ios::app);
        f << entry.dump() << "\n";
    }

private:
    std::string path_;
    std::mutex mtx_;
};

std::unique_ptr<Logger> g_logger;

struct RunResult {
    int exit_code = -1;
    std::string out;
    std::string err;
};

void read_all(int fd, std::string& out) {
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) out.append(buf, n);
}

void send_line(int fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = write(fd, data.data() + off, data.size() - off);
        if (n <= 0) return;  // peer gone; nothing more we can do
        off += static_cast<size_t>(n);
    }
}

// Runs `claude` with the given args via fork/exec (no shell involved, so
// prompt text can never be interpreted as shell syntax).
RunResult run_claude(const std::vector<std::string>& args) {
    RunResult res;
    int outp[2], errp[2];
    if (pipe(outp) != 0 || pipe(errp) != 0) {
        res.err = "pipe() failed";
        return res;
    }
    pid_t pid = fork();
    if (pid < 0) {
        res.err = "fork() failed";
        return res;
    }
    if (pid == 0) {
        dup2(outp[1], STDOUT_FILENO);
        dup2(errp[1], STDERR_FILENO);
        close(outp[0]); close(outp[1]);
        close(errp[0]); close(errp[1]);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>("claude"));
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp("claude", argv.data());
        _exit(127);
    }
    close(outp[1]);
    close(errp[1]);
    std::thread t_out([&] { read_all(outp[0], res.out); });
    std::thread t_err([&] { read_all(errp[0], res.err); });
    t_out.join();
    t_err.join();
    close(outp[0]);
    close(errp[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    res.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return res;
}

// One-time calibration: figure out which ~/.claude/projects/<...>/ directory
// this server's cwd maps to, without reimplementing Claude Code's internal
// path-encoding scheme. Costs one tiny throwaway API call at startup.
bool discover_project_dir(std::string& err) {
    std::string uuid = gen_uuid();
    RunResult r = run_claude({"-p", "reply with exactly: OK", "--output-format", "json",
                               "--session-id", uuid});
    if (r.exit_code != 0) {
        err = "claude invocation failed (exit " + std::to_string(r.exit_code) + "): " + r.err;
        return false;
    }
    try {
        json j = json::parse(r.out);
        if (j.value("is_error", false)) {
            err = "calibration call reported an error: " + r.out;
            return false;
        }
    } catch (...) {
        err = "failed to parse claude output during calibration: " + r.out;
        return false;
    }
    const char* home = getenv("HOME");
    if (!home) {
        err = "HOME not set";
        return false;
    }
    fs::path projects = fs::path(home) / ".claude" / "projects";
    std::error_code ec;
    for (auto& entry : fs::directory_iterator(projects, ec)) {
        if (!entry.is_directory()) continue;
        fs::path candidate = entry.path() / (uuid + ".jsonl");
        if (fs::exists(candidate)) {
            g_project_dir = entry.path().string();
            fs::remove(candidate, ec);  // clean up the calibration session
            return true;
        }
    }
    err = "could not locate project directory after calibration call";
    return false;
}

struct SessionInfo {
    std::string name, id, created, last_used;
};

class Registry {
public:
    explicit Registry(std::string path) : path_(std::move(path)) { load(); }

    std::vector<SessionInfo> list() {
        std::lock_guard<std::mutex> lk(mtx_);
        return sessions_;
    }

    size_t count() {
        std::lock_guard<std::mutex> lk(mtx_);
        return sessions_.size();
    }

    bool create(const std::string& name, std::string& id_out, std::string& err) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (find(name)) {
            err = "session name already exists";
            return false;
        }
        if (g_cfg.max_sessions > 0 && static_cast<int>(sessions_.size()) >= g_cfg.max_sessions) {
            err = "session limit reached (" + std::to_string(g_cfg.max_sessions) +
                  "); delete an old one first";
            return false;
        }
        SessionInfo s;
        s.name = name;
        s.id = gen_uuid();
        s.created = now_iso();
        s.last_used = s.created;
        id_out = s.id;
        sessions_.push_back(s);
        save();
        return true;
    }

    bool get_id(const std::string& name, std::string& id_out) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto* s = find(name);
        if (!s) return false;
        id_out = s->id;
        return true;
    }

    void touch(const std::string& name) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto* s = find(name);
        if (s) {
            s->last_used = now_iso();
            save();
        }
    }

    bool remove(const std::string& name, std::string& id_out) {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
            if (it->name == name) {
                id_out = it->id;
                sessions_.erase(it);
                save();
                return true;
            }
        }
        return false;
    }

    // Removes every session whose last_used is older than max_age_days.
    // Returns the removed sessions (name+id) so the caller can delete their
    // transcript files and log what happened.
    std::vector<SessionInfo> evict_older_than(int max_age_days) {
        std::vector<SessionInfo> evicted;
        if (max_age_days <= 0) return evicted;
        long long cutoff = static_cast<long long>(std::time(nullptr)) - static_cast<long long>(max_age_days) * 86400;
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            long long last = parse_iso_to_epoch(it->last_used);
            if (last >= 0 && last < cutoff) {
                evicted.push_back(*it);
                it = sessions_.erase(it);
            } else {
                ++it;
            }
        }
        if (!evicted.empty()) save();
        return evicted;
    }

private:
    SessionInfo* find(const std::string& name) {
        for (auto& s : sessions_)
            if (s.name == name) return &s;
        return nullptr;
    }

    void load() {
        std::ifstream f(path_);
        if (!f) return;
        json j;
        try {
            f >> j;
        } catch (...) {
            return;
        }
        for (auto& item : j) {
            SessionInfo s;
            s.name = item.value("name", "");
            s.id = item.value("id", "");
            s.created = item.value("created", "");
            s.last_used = item.value("last_used", "");
            sessions_.push_back(s);
        }
    }

    void save() {
        json j = json::array();
        for (auto& s : sessions_)
            j.push_back({{"name", s.name}, {"id", s.id}, {"created", s.created}, {"last_used", s.last_used}});
        std::ofstream f(path_, std::ios::trunc);
        f << j.dump(2);
    }

    std::string path_;
    std::vector<SessionInfo> sessions_;
    std::mutex mtx_;
};

// Serializes concurrent prompts against the *same* session id, so two
// overlapping requests can't race on the same transcript file.
std::mutex g_locks_mtx;
std::map<std::string, std::shared_ptr<std::mutex>> g_session_locks;

std::shared_ptr<std::mutex> get_session_lock(const std::string& id) {
    std::lock_guard<std::mutex> lk(g_locks_mtx);
    auto it = g_session_locks.find(id);
    if (it != g_session_locks.end()) return it->second;
    auto m = std::make_shared<std::mutex>();
    g_session_locks[id] = m;
    return m;
}

json handle_list(Registry& reg) {
    json arr = json::array();
    for (auto& s : reg.list())
        arr.push_back({{"name", s.name}, {"id", s.id}, {"created", s.created}, {"last_used", s.last_used}});
    return {{"ok", true}, {"sessions", arr}};
}

json handle_create(Registry& reg, const std::string& name) {
    if (name.empty()) return {{"ok", false}, {"error", "name required"}};
    std::string id, err;
    if (!reg.create(name, id, err)) return {{"ok", false}, {"error", err}};
    return {{"ok", true}, {"name", name}, {"id", id}};
}

json handle_delete(Registry& reg, const std::string& name) {
    std::string id;
    if (!reg.remove(name, id)) return {{"ok", false}, {"error", "no such session"}};
    std::error_code ec;
    fs::remove(transcript_path(id), ec);
    return {{"ok", true}};
}

json handle_prompt(Registry& reg, const std::string& name, const std::string& text) {
    std::string id;
    if (!reg.get_id(name, id)) return {{"ok", false}, {"error", "no such session"}};
    if (text.empty()) return {{"ok", false}, {"error", "prompt text required"}};

    auto lock = get_session_lock(id);
    std::lock_guard<std::mutex> lk(*lock);

    bool is_first = !fs::exists(transcript_path(id));
    std::vector<std::string> args = {"-p", text, "--output-format", "json", "--dangerously-skip-permissions"};
    if (is_first) {
        args.push_back("--session-id");
        args.push_back(id);
    } else {
        args.push_back("--resume");
        args.push_back(id);
    }

    RunResult r = run_claude(args);
    if (r.exit_code != 0)
        return {{"ok", false}, {"error", "claude exited with code " + std::to_string(r.exit_code) + ": " + r.err}};

    json cj;
    try {
        cj = json::parse(r.out);
    } catch (...) {
        return {{"ok", false}, {"error", "failed to parse claude output"}};
    }

    reg.touch(name);
    return {{"ok", true},
            {"result", cj.value("result", "")},
            {"is_error", cj.value("is_error", false)},
            {"cost_usd", cj.value("total_cost_usd", 0.0)}};
}

// RAII guard so the active-connection counter is decremented on every exit
// path (return, exception, thread end) without repeating the decrement.
class ConnGuard {
public:
    explicit ConnGuard(std::atomic<int>& counter) : counter_(counter) {}
    ~ConnGuard() { counter_.fetch_sub(1); }

private:
    std::atomic<int>& counter_;
};

void handle_client(int fd, Registry* reg, std::string peer, std::atomic<int>* active) {
    ConnGuard guard(*active);

    if (g_cfg.read_timeout_sec > 0) {
        struct timeval tv{};
        tv.tv_sec = g_cfg.read_timeout_sec;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    std::string buf;
    char tmp[4096];
    while (true) {
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                g_logger->log({{"event", "timeout"}, {"peer", peer}});
            }
            break;
        }
        if (n == 0) break;
        buf.append(tmp, n);

        if (buf.size() > g_cfg.max_line_bytes && buf.find('\n') == std::string::npos) {
            json resp = {{"ok", false}, {"error", "line too long"}};
            send_line(fd, resp.dump() + "\n");
            g_logger->log({{"event", "line_too_long"}, {"peer", peer}});
            close(fd);
            return;
        }

        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (line.empty()) continue;

            json req;
            try {
                req = json::parse(line);
            } catch (...) {
                send_line(fd, json({{"ok", false}, {"error", "invalid json"}}).dump() + "\n");
                continue;
            }

            if (req.value("token", "") != g_cfg.token) {
                send_line(fd, json({{"ok", false}, {"error", "unauthorized"}}).dump() + "\n");
                g_logger->log({{"event", "unauthorized"}, {"peer", peer}});
                close(fd);
                return;
            }

            std::string cmd = req.value("cmd", "");
            std::string name = req.value("name", "");
            json resp;
            if (cmd == "list") {
                resp = handle_list(*reg);
            } else if (cmd == "create") {
                resp = handle_create(*reg, name);
            } else if (cmd == "delete") {
                resp = handle_delete(*reg, name);
            } else if (cmd == "prompt") {
                std::string text = req.value("text", "");
                resp = handle_prompt(*reg, name, text);
                g_logger->log({{"event", "prompt"},
                                {"peer", peer},
                                {"session", name},
                                {"prompt_bytes", text.size()},
                                {"ok", resp.value("ok", false)},
                                {"cost_usd", resp.value("cost_usd", 0.0)}});
            } else {
                resp = {{"ok", false}, {"error", "unknown cmd"}};
            }

            if (cmd == "create" || cmd == "delete")
                g_logger->log({{"event", cmd}, {"peer", peer}, {"session", name}, {"ok", resp.value("ok", false)}});

            send_line(fd, resp.dump() + "\n");
        }
    }
    close(fd);
}

bool load_config(const std::string& path, std::string& err) {
    std::ifstream f(path);
    if (!f) {
        err = "cannot open config file: " + path;
        return false;
    }
    json j;
    try {
        f >> j;
    } catch (std::exception& e) {
        err = std::string("invalid config json: ") + e.what();
        return false;
    }
    if (!j.contains("bind_ip") || !j.contains("port") || !j.contains("token")) {
        err = "config must set bind_ip, port, and token";
        return false;
    }
    g_cfg.bind_ip = j.value("bind_ip", "");
    g_cfg.port = j.value("port", 0);
    g_cfg.token = j.value("token", "");
    g_cfg.project_workdir = j.value("project_workdir", g_cfg.project_workdir);
    g_cfg.registry_path = j.value("registry_path", g_cfg.registry_path);
    g_cfg.log_path = j.value("log_path", g_cfg.log_path);
    g_cfg.max_connections = j.value("max_connections", g_cfg.max_connections);
    g_cfg.read_timeout_sec = j.value("read_timeout_sec", g_cfg.read_timeout_sec);
    g_cfg.max_line_bytes = j.value("max_line_bytes", g_cfg.max_line_bytes);
    g_cfg.session_max_age_days = j.value("session_max_age_days", g_cfg.session_max_age_days);
    g_cfg.max_sessions = j.value("max_sessions", g_cfg.max_sessions);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <config.json>\n";
        std::cerr << "see config.example.json for the format\n";
        return 1;
    }

    std::string err;
    if (!load_config(argv[1], err)) {
        std::cerr << "FATAL: " << err << "\n";
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    if (!g_cfg.project_workdir.empty()) {
        std::error_code ec;
        fs::current_path(g_cfg.project_workdir, ec);
        if (ec) {
            std::cerr << "FATAL: cannot chdir to project_workdir '" << g_cfg.project_workdir << "': " << ec.message()
                       << "\n";
            return 1;
        }
    }

    g_logger = std::make_unique<Logger>(g_cfg.log_path);

    std::cerr << "Calibrating project directory (one-time throwaway claude call)...\n";
    if (!discover_project_dir(err)) {
        std::cerr << "FATAL: " << err << "\n";
        return 1;
    }
    std::cerr << "Project directory: " << g_project_dir << "\n";

    Registry reg(g_cfg.registry_path);

    if (g_cfg.session_max_age_days > 0) {
        auto evicted = reg.evict_older_than(g_cfg.session_max_age_days);
        for (auto& s : evicted) {
            std::error_code ec;
            fs::remove(transcript_path(s.id), ec);
            g_logger->log({{"event", "evicted"}, {"session", s.name}, {"last_used", s.last_used}});
            std::cerr << "Evicted stale session '" << s.name << "' (last used " << s.last_used << ")\n";
        }
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(g_cfg.port);
    if (inet_pton(AF_INET, g_cfg.bind_ip.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "invalid bind ip: " << g_cfg.bind_ip << "\n";
        return 1;
    }
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        perror("bind");
        return 1;
    }
    if (listen(sock, 16) != 0) {
        perror("listen");
        return 1;
    }
    std::cerr << "Listening on " << g_cfg.bind_ip << ":" << g_cfg.port << "\n";
    g_logger->log({{"event", "startup"}, {"bind_ip", g_cfg.bind_ip}, {"port", g_cfg.port}});

    std::atomic<int> active_connections{0};

    while (true) {
        sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);
        int cfd = accept(sock, reinterpret_cast<sockaddr*>(&client_addr), &len);
        if (cfd < 0) continue;

        char ipbuf[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &client_addr.sin_addr, ipbuf, sizeof(ipbuf));
        std::string peer = std::string(ipbuf) + ":" + std::to_string(ntohs(client_addr.sin_port));

        if (active_connections.load() >= g_cfg.max_connections) {
            send_line(cfd, json({{"ok", false}, {"error", "server busy, try again"}}).dump() + "\n");
            g_logger->log({{"event", "rejected_busy"}, {"peer", peer}});
            close(cfd);
            continue;
        }

        active_connections.fetch_add(1);
        g_logger->log({{"event", "connect"}, {"peer", peer}});
        std::thread(handle_client, cfd, &reg, peer, &active_connections).detach();
    }
}
