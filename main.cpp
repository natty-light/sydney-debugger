#include <cstring>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace ftxui;
using json = nlohmann::json;

class Conn {

private:
  std::string _buf;
  int _sock;

public:
  Conn(std::string &path) {
    _sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (_sock == -1) {
      return;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    int cn = connect(_sock, (struct sockaddr *)&addr, sizeof(addr));
    if (cn == -1) {
      return;
    }
  }

  int sendCmd(std::string &msg) {
    std::string line = msg + "\n";
    return write(_sock, line.c_str(), line.size());
  }

  std::string readEvent() {
    size_t pos = _buf.find('\n');
    if (pos != std::string::npos) {
      std::string line = _buf.substr(0, pos);
      _buf.erase(0, pos + 1);
      return line;
    }

    char chunk[4096];
    while (true) {
      ssize_t n = read(_sock, chunk, sizeof(chunk));
      if (n <= 0)
        return "";
      _buf.append(chunk, n);
      pos = _buf.find('\n');
      if (pos != std::string::npos) {
        std::string line = _buf.substr(0, pos);
        _buf.erase(0, pos + 1);
        return line;
      }
    }
  }

  ~Conn() { close(_sock); }
};

struct DebugState {
  std::string event;
  std::string file;
  int line = 0;
  std::string reason;
};

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::cerr << "usage sydney-dbg <socket-path>\n";
    return 1;
  }

  std::string path(argv[1]);
  Conn conn(path);

  std::mutex mtx;
  std::string status = "waiting...";
  struct DebugState state;

  auto screen = ScreenInteractive::Fullscreen();

  std::thread reader([&] {
    while (true) {
      auto line = conn.readEvent();
      auto j = json::parse(line);
      std::string event = j["event"];
      struct DebugState newState;

      if (event == "stopped") {
        newState.file = j["file"];
        newState.line = j["line"];
        newState.reason = j["reason"];
      }
      if (line.empty())
        break;
      std::lock_guard<std::mutex> lock(mtx);
      state = newState;
      screen.PostEvent(Event::Custom);
    }
  });

  auto component = Renderer([&] {
    std::lock_guard<std::mutex> lock(mtx);
    return vbox({
               text("sydney-dbg") | bold | center,
               separator(),
               text("stopped: " + state.file + ":" +
                    std::to_string(state.line) + "    " + state.reason) |
                   center,

           }) |
           border;
  });

  auto with_keys = CatchEvent(component, [&](Event event) {
    if (event == Event::Character('n')) {
      auto cmd = json({{"cmd", "step_line"}}).dump();
      conn.sendCmd(cmd);
      return true;
    }

    if (event == Event::Character('c')) {
      auto cmd = json({{"cmd", "continue"}}).dump();
      conn.sendCmd(cmd);
      return true;
    }

    if (event == Event::Character('q')) {
      screen.Exit();
      return true;
    }

    return false;
  });

  screen.Loop(with_keys);
  return 0;
}
