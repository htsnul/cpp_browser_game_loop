# C++でブラウザを入出力に利用してゲームループ

## 概要

C++でゲーム的なものを作ろうとすると、キー入力やグラフィックの描画など、
入出力をどうするかが課題となる。
入出力にブラウザが利用できれば汎用性が高そうだ。

今だとWasmなどもあるが、専用ツールなども必要になってしまう。
Linuxの素のC++だけでできることを考えたい。

## アイデア

1. C++でローカルサーバーを動かす
2. ブラウザでアクセスする
3. キー状態を送信し画像を受信するHTML、を送信する
4. ローカルサーバーは、要求を受けたら画像を生成して応答する

## 動作の様子

ブラウザ上での矢印キー操作で赤い四角が移動できる。

## 速度が出るのか？

1フレに1回サーバーとやりとりをするわけで、その水準の速度が出るのか気になる。

[Streaming requests with the fetch API](https://developer.chrome.com/docs/capabilities/web-apis/fetch-streaming-requests)
や、WebSocketなどが使えればいけそうだが、
非HTTPSやHTTP/1.xでは使えなかったりブラウザが対応していなかったり、
ローカルサーバーでは簡単には使えなさそうだし、汎用性も下がってしまう。

しかしHTTP/1.1であっても、Keep-Aliveと言う機構がある。
これは、一度接続すると後続の送受信で同じ接続が使いまわせるようになっている。

本記事の実装で、60FPSが実現できる程度の速度は少なくとも手元では出るようだった。

## コード

https://github.com/htsnul/cpp_browser_game_loop/blob/main/main.cpp

## 全体の流れ

```c++
int main() {
  const int sockFd = createSocketFileDescriptor();
  if (sockFd == -1) return EXIT_FAILURE;
  runConnectionLoop(sockFd);
  return 0;
}
```

ソケットのファイルデスクリプタを作ったら、
あとは接続のループに入る。

## ソケットのファイルデスクリプタ作成

```c++
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
```

このあたりは雛形のようなものだろう。

## 接続ループ

```c++
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
```

`accept` で接続を待つ。
ブラウザで、`http://localhost:8080/` にアクセスすると接続が確立し、
後続の `runReceiveSendLoop` に入る。

## 送信受信ループ

```c++
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
    send(acceptedFd, response.c_str(), response.size(), 0);
  }
}
```

`recv` で受信待ちをして、受信できたら応答を生成して `send` で送信する。
先述したようにKeep-Aliveにより、基本は接続が維持されるのでこのループが回り続ける。

ブラウザ側でページを閉じるなどで、接続が切れる場合がある。
これは `readSize == 0` で検出できるので、
これを検出したら、関数を抜けて、接続ループに戻る。

## 応答の生成

```c++
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
    update(requestBody);
    std::string str;
    std::copy(
      canvas.data.begin(), canvas.data.end(),
      std::back_inserter(str)
    );
    return str;
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
```

GET要求が来たら、基本となるHTML `baseHTML` を応答とする。
このHTML（内容は後述）からPOST要求が来るので、そのときは、
`update` を呼び、生成した画像のバイナリを文字列としてコピーして返却する。

## 基本となるHTML

```c++
const auto baseHTML = R"(
<!DOCTYPE html>
<canvas width="256" height="256"></canvas>
<script>
  const keys = new Uint8Array(256);
  onkeydown = (e) => keys[e.keyCode] = 1;
  onkeyup = (e) => keys[e.keyCode] = 0;
  onload = async () => {
    while (true) {
      const res = await fetch("/", { method: "POST", body: keys });
      const arrayBuffer = await res.arrayBuffer();
      const canvas = document.querySelector("canvas");
      const ctx = canvas.getContext("2d");
      const imageData = new ImageData(new Uint8ClampedArray(arrayBuffer), 256);
      await new Promise((resolve) => requestAnimationFrame(() => {
        ctx.putImageData(imageData, 0, 0);
        resolve();
      }));
    }
  };
</script>
)";
```

キー情報を配列として管理し、それを `fetch` の `body` としてPOSTで送信する。
これにより、C++側ではキー情報を取得できるようになる。

`fetch` の応答をそのまま `ImageData` として、`canvas` に設定している。
これにより、C++側で生成した画像が表示される。

ただし、

```
      await new Promise((resolve) => requestAnimationFrame(() => {
        ctx.putImageData(imageData, 0, 0);
        resolve();
      }));
```

により、垂直同期を待機し画像更新するようにしている。

これがループで実行される。

## キー入力の取得

要求の `body` にそのままキー情報が `0`、`1` のバイナリで入っているので、

```c++
enum class KeyCode {
  ArrowLeft = 37,
  ArrowUp = 38,
  ArrowRight = 39,
  ArrowDown = 40,
};

bool isKeyDown(std::string_view requestBody, KeyCode keyCode) {
  return requestBody[int(keyCode)];
}
```

と用意しておき、

```c++
    if (isKeyDown(requestBody, KeyCode::ArrowLeft)) /* ... */;
```

のようにすることで、キー入力を判定することができる。

## 画像の出力

HTMLのcanvasにそのまま設定できるバイナリを返したい。

```c++
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
  std::vector<uint8_t> data;
};
```

先述のように、`data()` をそのまま文字列にコピーして返せばよい。

## 更新関数

ここまでの機能を組み合わせれば、

```c++
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

void update(std::string requestBody) {
  hero.update(requestBody);
  canvas.clear();
  hero.draw();
}
```

で、入力情報を使って更新描画を行い、結果を返すことができる。

## まとめ

ブラウザを利用することで、Linux上の素のC++でも簡単なゲームループを実現できた。
Web Audio向けのデータも送信すればC++で生成した波形を鳴らすこともできそうだ。

