// remclaude client: connects to the remclaude server and presents a simple
// menu to pick / create / delete a named Claude Code session, then drops
// into a prompt loop for the chosen session.
//
// Usage: client <server_ip> <port> <token>
//    or: client <server_ip> <port> @<token_file>   (reads the token's first
//        line from a file instead of taking it as a plain argument, so it
//        doesn't linger in shell history)

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

using json = nlohmann::json;

namespace {

class Conn {
public:
    bool connect_to(const std::string& ip, int port) {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) return false;
        return connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    }

    json request(const json& req) {
        std::string line = req.dump() + "\n";
        if (write(fd_, line.data(), line.size()) < 0)
            return {{"ok", false}, {"error", "write failed"}};
        while (true) {
            auto pos = buf_.find('\n');
            if (pos != std::string::npos) {
                std::string resp_line = buf_.substr(0, pos);
                buf_.erase(0, pos + 1);
                try {
                    return json::parse(resp_line);
                } catch (...) {
                    return {{"ok", false}, {"error", "bad response from server"}};
                }
            }
            char tmp[4096];
            ssize_t n = read(fd_, tmp, sizeof(tmp));
            if (n <= 0) return {{"ok", false}, {"error", "connection closed"}};
            buf_.append(tmp, n);
        }
    }

    ~Conn() {
        if (fd_ >= 0) close(fd_);
    }

private:
    int fd_ = -1;
    std::string buf_;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: " << argv[0] << " <server_ip> <port> <token>\n";
        return 1;
    }
    std::string ip = argv[1];
    int port = std::atoi(argv[2]);
    std::string token = argv[3];
    if (!token.empty() && token[0] == '@') {
        std::ifstream tf(token.substr(1));
        if (!tf) {
            std::cerr << "cannot open token file: " << token.substr(1) << "\n";
            return 1;
        }
        std::getline(tf, token);
    }

    Conn conn;
    if (!conn.connect_to(ip, port)) {
        std::cerr << "connect to " << ip << ":" << port << " failed\n";
        return 1;
    }

    std::string current_name;

    // Returns false when the user chose to quit.
    auto show_menu = [&]() -> bool {
        json resp = conn.request({{"token", token}, {"cmd", "list"}});
        if (!resp.value("ok", false)) {
            std::cerr << "error: " << resp.value("error", "?") << "\n";
            return false;
        }
        auto sessions = resp["sessions"];
        std::cout << "\n=== Sessions ===\n";
        int i = 1;
        for (auto& s : sessions) {
            std::cout << "  " << i++ << ") " << s.value("name", "?")
                       << "  (last used: " << s.value("last_used", "-") << ")\n";
        }
        std::cout << "  n) New session\n";
        std::cout << "  d) Delete a session\n";
        std::cout << "  q) Quit\n> ";

        std::string choice;
        if (!std::getline(std::cin, choice)) return false;

        if (choice == "q") return false;

        if (choice == "n") {
            std::cout << "New session name: ";
            std::string name;
            std::getline(std::cin, name);
            json cr = conn.request({{"token", token}, {"cmd", "create"}, {"name", name}});
            if (!cr.value("ok", false)) {
                std::cerr << "error: " << cr.value("error", "?") << "\n";
                return true;
            }
            current_name = name;
            return true;
        }

        if (choice == "d") {
            std::cout << "Session name to delete: ";
            std::string name;
            std::getline(std::cin, name);
            json dr = conn.request({{"token", token}, {"cmd", "delete"}, {"name", name}});
            if (!dr.value("ok", false))
                std::cerr << "error: " << dr.value("error", "?") << "\n";
            else
                std::cout << "deleted.\n";
            return true;
        }

        try {
            int idx = std::stoi(choice);
            if (idx >= 1 && idx <= static_cast<int>(sessions.size())) {
                current_name = sessions[idx - 1].value("name", "");
                return true;
            }
        } catch (...) {
        }
        std::cerr << "invalid choice\n";
        return true;
    };

    while (current_name.empty()) {
        if (!show_menu()) return 0;
    }

    std::cout << "\n--- Session: " << current_name << " (:menu to switch, :quit to exit) ---\n";
    while (true) {
        std::cout << "\n[" << current_name << "] > ";
        std::string prompt;
        if (!std::getline(std::cin, prompt)) break;
        if (prompt == ":quit") break;
        if (prompt == ":menu") {
            current_name.clear();
            while (current_name.empty()) {
                if (!show_menu()) return 0;
            }
            std::cout << "\n--- Session: " << current_name << " ---\n";
            continue;
        }
        if (prompt.empty()) continue;

        json resp = conn.request({{"token", token}, {"cmd", "prompt"}, {"name", current_name}, {"text", prompt}});
        if (!resp.value("ok", false)) {
            std::cerr << "error: " << resp.value("error", "?") << "\n";
            continue;
        }
        std::cout << "\n" << resp.value("result", "") << "\n";
        if (resp.value("is_error", false)) std::cerr << "(claude reported an error)\n";
    }
    return 0;
}
