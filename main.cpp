#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <iomanip>
#include <optional>
#include <string>
#include <sstream>
#include <thread>
#include <vector>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

using namespace std::chrono_literals;
using namespace std::string_literals;

const auto baseHTML = R"(
<!DOCTYPE html>
<canvas width="256" height="256"></canvas>
<script>
  const keys = Array(256).fill(0);
  onkeydown = (e) => keys[e.keyCode] = 1;
  onkeyup = (e) => keys[e.keyCode] = 0;
  onload = async () => {
    while (true) {
      const res = await fetch("/", { method: "POST", body: keys.join("") });
      const json = await res.json();
      const canvas = document.querySelector("canvas");
      const ctx = canvas.getContext("2d");
      const imageData = new ImageData(new Uint8ClampedArray(json), 256);
      ctx.putImageData(imageData, 0, 0);
    }
  };
</script>
)";

enum class KeyCode {
  ArrowLeft = 37,
  ArrowUp = 38,
  ArrowRight = 39,
  ArrowDown = 40,
};

bool isKeyDown(std::string_view requestBody, KeyCode keyCode) {
  return requestBody[int(keyCode)] == '1';
}

struct Color {
  Color(): Color(0, 0, 0) {}
  Color(uint8_t r, uint8_t g, uint8_t b) {
    reinterpret_cast<uint8_t*>(&data)[0] = r;
    reinterpret_cast<uint8_t*>(&data)[1] = g;
    reinterpret_cast<uint8_t*>(&data)[2] = b;
    reinterpret_cast<uint8_t*>(&data)[3] = 255;
  }
  uint32_t data{};
};

struct Canvas {
  static constexpr int WIDTH{256};
  static constexpr int HEIGHT{256};
  static constexpr int DEPTH{4};
  Canvas() { data.resize(WIDTH * HEIGHT * DEPTH); }
  void setPixel(int x, int y, Color c) {
    *reinterpret_cast<uint32_t*>(&data[(x + y * WIDTH) * DEPTH]) = c.data;
  }
  void drawRect(int x, int y, int w, int h, Color c) {
    const int cx = std::clamp(x, 0, WIDTH);
    const int cy = std::clamp(y, 0, HEIGHT);
    const int cw = std::min(x + w, WIDTH) - cx;
    const int ch = std::min(y + h, HEIGHT) - cy;
    for (int yi = cy; yi < cy + ch; ++yi) {
      for (int xi = cx; xi < cx + cw; ++xi) {
        setPixel(xi, yi, c);
      }
    }
  }
  void clear() {
    drawRect(0, 0, WIDTH, HEIGHT, Color{0, 0, 0});
  }
  std::string toString() {
    std::string str;
    for (int i = 0; i < data.size(); ++i) {
      str += std::to_string(data[i]);
      if (i != data.size() - 1) str += ",";
    }
    return "[" + str + "]";
  }
  std::vector<uint8_t> data;
};

Canvas canvas;

struct Hero {
  void update(std::string_view requestBody) {
    const float spd = 8.0f;
    if (isKeyDown(requestBody, KeyCode::ArrowLeft)) x_ -= spd;
    if (isKeyDown(requestBody, KeyCode::ArrowUp)) y_ -= spd;
    if (isKeyDown(requestBody, KeyCode::ArrowRight)) x_ += spd;
    if (isKeyDown(requestBody, KeyCode::ArrowDown)) y_ += spd;
  }
  void draw() {
    const float r = 4.0f;
    canvas.drawRect(x_ - r, y_ - r, 2.0f * r , 2.0f * r, Color{255, 0, 0});
  }
  float x_{0.5f * Canvas::WIDTH};
  float y_{0.5f * Canvas::HEIGHT};
};

Hero hero;

std::string update(std::string requestBody) {
  hero.update(requestBody);
  canvas.clear();
  hero.draw();
  return canvas.toString();
}

std::string getRequestLine(std::string_view request) {
  return std::string(request.substr(0, request.find("\r\n")));
}

std::string getRequestBody(std::string_view request) {
  if (getRequestLine(request).find("POST") != 0) return {};
  return std::string(request.substr(request.find("\r\n\r\n") + 4));
}

std::string createResponseBody(std::string_view request) {
  const auto requestLine = getRequestLine(request);
  if (requestLine.find("GET / ") == 0) {
    return baseHTML;
  }
  if (requestLine.find("POST / ") == 0) {
    const auto requestBody = getRequestBody(request);
    return update(requestBody);
  }
  return {};
}

std::string createResponseHeader(size_t responseBodySize) {
  return {
    "HTTP/1.1 200 OK\r\n"s +
    "Content-Type: text/html\r\n"s +
    "Content-Length: "s + std::to_string(responseBodySize) + "\r\n"s
  };
}

std::string createResponse(std::string_view request) {
  const auto responseBody = createResponseBody(request);
  return createResponseHeader(responseBody.size()) + "\r\n" + std::string(responseBody);
}

int createSocketFileDescriptor() {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == -1) {
    std::perror(nullptr);
    return fd;
  }
  {
    const int flag = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) == -1) {
      std::perror(nullptr);
      return -1;
    }
  }
  {
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1) {
      std::perror(nullptr);
      return -1;
    }
  }
  if (listen(fd, 1) == -1) {
    std::perror(nullptr);
    return -1;
  }
  return fd;
}

void runReceiveSendLoop(int acceptedFd) {
  auto nextSendTimePoint = std::chrono::steady_clock::now();
  while(1) {
    const size_t RECV_BUF_SIZE{4096};
    std::vector<char> buf(RECV_BUF_SIZE);
    const ssize_t readSize = recv(acceptedFd, buf.data(), buf.size(), 0);
    buf.resize(readSize);
    if (readSize == -1) {
      std::perror(nullptr);
      break;
    }
    if (readSize == 0) {
      std::cout << "Peer shutdown" << std::endl;
      break;
    }
    std::string request = std::string(buf.data(), readSize);
    const auto response = createResponse(request);
    std::this_thread::sleep_until(nextSendTimePoint);
    send(acceptedFd, response.c_str(), response.size(), 0);
    nextSendTimePoint += 100ms;
  }
}

void runConnectionLoop(int sockFd) {
  while (true) {
    const int acceptedFd = accept(sockFd, nullptr, nullptr);
    if (acceptedFd == -1) {
      std::perror(nullptr);
      return;
    }
    runReceiveSendLoop(acceptedFd);
    close(acceptedFd);
  }
}

int main() {
  const int sockFd = createSocketFileDescriptor();
  if (sockFd == -1) return EXIT_FAILURE;
  runConnectionLoop(sockFd);
  return 0;
}

