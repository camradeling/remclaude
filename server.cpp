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
// Usage: server <bind_ip> <port> <token> [registry_path]
//
// Run this from the directory whose Claude Code project bucket you want
// sessions to live in (transcripts land under
// ~/.claude/projects/<encoded-cwd>/<session-id>.jsonl).

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

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
std::string g_token;

std::string now_iso() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return buf;
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

    bool create(const std::string& name, std::string& id_out, std::string& err) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (find(name)) {
            err = "session name already exists";
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

void handle_client(int fd, Registry* reg) {
    std::string buf;
    char tmp[4096];
    while (true) {
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n <= 0) break;
        buf.append(tmp, n);
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (line.empty()) continue;

            json req;
            try {
                req = json::parse(line);
            } catch (...) {
                json resp = {{"ok", false}, {"error", "invalid json"}};
                std::string out = resp.dump() + "\n";
                send_line(fd, out);
                continue;
            }

            if (req.value("token", "") != g_token) {
                json resp = {{"ok", false}, {"error", "unauthorized"}};
                std::string out = resp.dump() + "\n";
                send_line(fd, out);
                close(fd);
                return;
            }

            std::string cmd = req.value("cmd", "");
            json resp;
            if (cmd == "list") resp = handle_list(*reg);
            else if (cmd == "create") resp = handle_create(*reg, req.value("name", ""));
            else if (cmd == "delete") resp = handle_delete(*reg, req.value("name", ""));
            else if (cmd == "prompt") resp = handle_prompt(*reg, req.value("name", ""), req.value("text", ""));
            else resp = {{"ok", false}, {"error", "unknown cmd"}};

            std::string out = resp.dump() + "\n";
            send_line(fd, out);
        }
    }
    close(fd);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: " << argv[0] << " <bind_ip> <port> <token> [registry_path]\n";
        std::cerr << "  run from the directory you want sessions associated with\n";
        return 1;
    }
    std::string bind_ip = argv[1];
    int port = std::atoi(argv[2]);
    g_token = argv[3];
    std::string registry_path = argc > 4 ? argv[4] : "remclaude_sessions.json";

    signal(SIGPIPE, SIG_IGN);

    std::cerr << "Calibrating project directory (one-time throwaway claude call)...\n";
    std::string err;
    if (!discover_project_dir(err)) {
        std::cerr << "FATAL: " << err << "\n";
        return 1;
    }
    std::cerr << "Project directory: " << g_project_dir << "\n";

    Registry reg(registry_path);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_ip.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "invalid bind ip: " << bind_ip << "\n";
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
    std::cerr << "Listening on " << bind_ip << ":" << port << "\n";

    while (true) {
        sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);
        int cfd = accept(sock, reinterpret_cast<sockaddr*>(&client_addr), &len);
        if (cfd < 0) continue;
        std::thread(handle_client, cfd, &reg).detach();
    }
}
