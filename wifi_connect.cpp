// ESP32_RGB565_Chunked.ino
// Принимает POST /update-image с телом ровно 240*135*2 = 64800 байт (RGB565, little-endian)
// Читает и рисует изображение чанками строк, чтобы избежать OOM.

#include <WiFi.h>
#include <TFT_eSPI.h> // https://github.com/Bodmer/TFT_eSPI

// --- WiFi ---
const char* ssid = "ssod";
const char* password = "password";

// --- Изображение ---
const uint16_t WIDTH = 240;
const uint16_t HEIGHT = 135;
const size_t TOTAL_BYTES = (size_t)WIDTH * (size_t)HEIGHT * 2; // 64800

TFT_eSPI tft = TFT_eSPI();
WiFiServer server(8080);

// Параметры чтения
const unsigned long BODY_TIMEOUT_MS = 10000; // таймаут для чтения chunk'а
const int CHUNK_ROWS = 4; // сколько строк за раз читать/рисовать. Уменьшить до 1 если нужно экономить память.

String readHeaderLine(WiFiClient &client, unsigned long timeoutMs = 2000) {
  String line;
  unsigned long start = millis();
  while (true) {
    while (client.available()) {
      char c = client.read();
      if (c == '\n') {
        if (line.length() && line.charAt(line.length() - 1) == '\r') {
          line.remove(line.length() - 1);
        }
        return line;
      } else {
        line += c;
      }
    }
    if (millis() - start > timeoutMs) break;
    delay(1);
  }
  if (line.length() && line.charAt(line.length() - 1) == '\r') line.remove(line.length() - 1);
  return line;
}

bool readFully(WiFiClient &client, uint8_t* buf, size_t len, unsigned long timeoutMs) {
  size_t got = 0;
  unsigned long start = millis();
  while (got < len && (millis() - start) < timeoutMs) {
    if (client.available()) {
      int r = client.read(buf + got, len - got);
      if (r > 0) got += r;
    } else {
      delay(1);
    }
  }
  return got == len;
}

void sendSimpleResponse(WiFiClient &client, int code, const char* body) {
  String status;
  if (code == 200) status = "HTTP/1.1 200 OK\r\n";
  else if (code == 400) status = "HTTP/1.1 400 Bad Request\r\n";
  else if (code == 404) status = "HTTP/1.1 404 Not Found\r\n";
  else status = "HTTP/1.1 500 Internal Server Error\r\n";

  String resp = status;
  resp += "Content-Type: text/plain\r\n";
  resp += "Content-Length: ";
  resp += String(strlen(body));
  resp += "\r\n\r\n";
  resp += body;
  client.print(resp);
}

void setup() {
  Serial.begin(115200);
  delay(100);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  Serial.print("Connected. IP: ");
  Serial.println(WiFi.localIP());

  server.begin();

  tft.init();
  tft.setRotation(1); // подбери нужную ориентацию
  tft.fillScreen(TFT_BLACK);
  Serial.println("Server started, waiting for connections...");
}

void loop() {
  WiFiClient client = server.available();
  if (!client) {
    delay(10);
    return;
  }

  Serial.println("Client connected");
  client.setTimeout(1);

  String requestLine = readHeaderLine(client);
  requestLine.trim();
  Serial.println("Request: " + requestLine);
  if (requestLine.length() == 0) {
    client.stop();
    Serial.println("Empty request");
    return;
  }

  if (!requestLine.startsWith("POST ") || requestLine.indexOf("/update-image") < 0) {
    sendSimpleResponse(client, 404, "Not found");
    client.stop();
    Serial.println("Not target path");
    return;
  }

  // Прочитать заголовки
  size_t contentLength = 0;
  while (true) {
    String h = readHeaderLine(client);
    if (h.length() == 0) break;
    String lh = h;
    lh.toLowerCase();
    if (lh.startsWith("content-length:")) {
      String v = h.substring(h.indexOf(':') + 1);
      v.trim();
      contentLength = (size_t)v.toInt();
      Serial.print("Content-Length: ");
      Serial.println(contentLength);
    }
  }

  if (contentLength != TOTAL_BYTES) {
    String msg = "Expected " + String(TOTAL_BYTES) + " bytes, got " + String(contentLength);
    sendSimpleResponse(client, 400, msg.c_str());
    client.stop();
    Serial.println("Bad Content-Length");
    return;
  }

  // Выделяем буферы для чанка (heap один раз)
  const int rowsPerChunk = CHUNK_ROWS;
  const size_t bytesPerChunk = (size_t)WIDTH * rowsPerChunk * 2;
  const size_t wordsPerChunk = (size_t)WIDTH * rowsPerChunk;

  uint8_t *byteBuf = (uint8_t*)malloc(bytesPerChunk);
  if (!byteBuf) {
    sendSimpleResponse(client, 500, "OOM byteBuf");
    client.stop();
    Serial.println("OOM allocating byteBuf; free heap: " + String(ESP.getFreeHeap()));
    return;
  }
  uint16_t *wordBuf = (uint16_t*)malloc(wordsPerChunk * sizeof(uint16_t));
  if (!wordBuf) {
    free(byteBuf);
    sendSimpleResponse(client, 500, "OOM wordBuf");
    client.stop();
    Serial.println("OOM allocating wordBuf; free heap: " + String(ESP.getFreeHeap()));
    return;
  }

  Serial.print("Begin receiving in chunks (rows per chunk = ");
  Serial.print(rowsPerChunk);
  Serial.println(")");

  bool error = false;
  for (uint16_t y = 0; y < HEIGHT; y += rowsPerChunk) {
    int rowsThis = min(rowsPerChunk, (int)(HEIGHT - y));
    size_t bytesNeeded = (size_t)WIDTH * rowsThis * 2;

    if (!readFully(client, byteBuf, bytesNeeded, BODY_TIMEOUT_MS)) {
      sendSimpleResponse(client, 408, "Timeout receiving body");
      Serial.println("Timeout receiving chunk at row " + String(y));
      error = true;
      break;
    }

    // Конвертируем в uint16_t (little-endian)
    size_t pixelsInChunk = (size_t)WIDTH * rowsThis;
    for (size_t i = 0; i < pixelsInChunk; ++i) {
      uint8_t lo = byteBuf[i*2];
      uint8_t hi = byteBuf[i*2 + 1];
      wordBuf[i] = (uint16_t)lo | ((uint16_t)hi << 8);
    }

    // Рисуем chunk
    tft.pushImage(0, y, WIDTH, rowsThis, wordBuf);
  }

  free(byteBuf);
  free(wordBuf);

  if (!error) {
    sendSimpleResponse(client, 200, "OK");
    Serial.println("Image updated successfully");
  }
  client.stop();
}
