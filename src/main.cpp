#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

#if __has_include("build_env.h")
#include "build_env.h"
#else
#define C64U_WIFI_SSID ""
#define C64U_WIFI_PASSWORD ""
#define C64U_TARGET_HOST "10.0.0.9"
#define C64U_TARGET_PASSWORD "karl"
#endif

#include "commodore_logo_rgb565.h"

namespace {

constexpr uint32_t kModalMs = 1400;
constexpr uint32_t kHttpTimeoutMs = 2500;
constexpr uint32_t kWiFiRetryMs = 10000;
constexpr uint32_t kFrameMs = 33;
constexpr uint32_t kDoublePressMs = 300;
constexpr uint32_t kHomeEffectMs = 5000;
constexpr uint32_t kHomeLongEffectMs = 7000;
constexpr uint32_t kHomeStaticMs = 1000;
constexpr size_t kMaxCpuChoices = 16;

constexpr const char* kMenuItems[] = {
    "Reset",
    "CPU Speed",
    "Connection Test",
    "Status",
};
constexpr size_t kMenuCount = sizeof(kMenuItems) / sizeof(kMenuItems[0]);

enum class ScreenMode : uint8_t {
  Home,
  Menu,
  CpuMenu,
  Status,
};

enum class HomeMode : uint8_t {
  Static,
  Water,
  RotoZoom,
  SineWave,
  RippleBump,
  RasterBars,
};

struct ApiResponse {
  bool transportOk = false;
  bool jsonOk = false;
  bool apiOk = false;
  int httpCode = -1;
  String body;
  String errors;
};

struct ConnectionState {
  bool wifiConnected = false;
  bool targetReachable = false;
  bool authOk = false;
  String detail = "Not tested";
};

struct HomeDemoState {
  HomeMode mode = HomeMode::Static;
  uint8_t nextEffectIndex = 0;
  uint32_t startedAtMs = 0;
  uint32_t pausedAtMs = 0;
  bool frameDirty = true;
};

struct AppState {
  ScreenMode screen = ScreenMode::Home;
  int menuIndex = 0;
  int cpuIndex = 0;
  String cpuCategory;
  String cpuItem;
  String currentCpuValue = "Unknown";
  bool cpuPathKnown = false;
  String cpuWireOptions[kMaxCpuChoices];
  String cpuDisplayOptions[kMaxCpuChoices];
  size_t cpuChoiceCount = 0;
  bool configReady = false;
  ConnectionState connection = {};
  String modalText;
  uint16_t modalColor = TFT_WHITE;
  uint32_t modalUntilMs = 0;
  bool lastModalVisible = false;
  uint32_t lastWiFiAttemptMs = 0;
  bool pendingSoftReset = false;
  uint32_t pendingSoftResetAtMs = 0;
  HomeDemoState home = {};
} app;

M5Canvas canvas(&M5.Display);
uint16_t* plainLogoPixels = nullptr;
uint16_t* boxedLogoPixels = nullptr;
uint16_t logoRow[240] = {};

int homeInnerX() { return 22; }
int homeInnerY() { return 20; }
int homeInnerW() { return 196; }
int homeInnerH() { return 95; }
String trimCopy(const String& value) {
  String result = value;
  result.trim();
  return result;
}

String configString(const char* value) {
  return trimCopy(value == nullptr ? "" : value);
}

const String& wifiSsid() {
  static const String value = configString(C64U_WIFI_SSID);
  return value;
}

const String& wifiPassword() {
  static const String value = configString(C64U_WIFI_PASSWORD);
  return value;
}

const String& targetHost() {
  static const String value = configString(C64U_TARGET_HOST);
  return value;
}

const String& targetPassword() {
  static const String value = configString(C64U_TARGET_PASSWORD);
  return value;
}

bool hasWiFiConfig() {
  return !wifiSsid().isEmpty();
}

bool hasTargetConfig() {
  return !targetHost().isEmpty();
}

bool configReady() {
  return hasWiFiConfig() && hasTargetConfig();
}

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

uint16_t plainBgColor() { return rgb565(34, 46, 114); }
uint16_t plainBgAccent() { return rgb565(56, 80, 170); }

uint16_t blend565(uint16_t from, uint16_t to, float t) {
  t = std::max(0.0f, std::min(1.0f, t));
  const uint8_t fromR = ((from >> 11) & 0x1Fu) << 3;
  const uint8_t fromG = ((from >> 5) & 0x3Fu) << 2;
  const uint8_t fromB = (from & 0x1Fu) << 3;
  const uint8_t toR = ((to >> 11) & 0x1Fu) << 3;
  const uint8_t toG = ((to >> 5) & 0x3Fu) << 2;
  const uint8_t toB = (to & 0x1Fu) << 3;
  return rgb565(static_cast<uint8_t>(fromR + (toR - fromR) * t),
                static_cast<uint8_t>(fromG + (toG - fromG) * t),
                static_cast<uint8_t>(fromB + (toB - fromB) * t));
}

void fitHeightRect(int srcW, int srcH, int dstX, int dstY, int dstW, int dstH, int* outX, int* outY, int* outW,
                   int* outH) {
  *outH = dstH;
  *outW = std::max(1, static_cast<int>((static_cast<float>(srcW) * static_cast<float>(dstH)) / static_cast<float>(srcH)));
  *outX = dstX + (dstW - *outW) / 2;
  *outY = dstY;
}

uint16_t logoSourcePixel(int x, int y) {
  x = std::max(0, std::min(kLogoSrcW - 1, x));
  y = std::max(0, std::min(kLogoSrcH - 1, y));
  return pgm_read_word(&commodore_logo_rgb565[y * kLogoSrcW + x]);
}

void drawLogoFitHeightToCanvas(M5Canvas& dst, int dstX, int dstY, int dstW, int dstH) {
  int fitX = 0;
  int fitY = 0;
  int fitW = 0;
  int fitH = 0;
  fitHeightRect(kLogoSrcW, kLogoSrcH, dstX, dstY, dstW, dstH, &fitX, &fitY, &fitW, &fitH);

  for (int y = 0; y < fitH; ++y) {
    const int dstRow = fitY + y;
    const int srcY = std::min(kLogoSrcH - 1, (y * kLogoSrcH) / fitH);
    for (int x = 0; x < fitW; ++x) {
      const int srcX = std::min(kLogoSrcW - 1, (x * kLogoSrcW) / fitW);
      dst.drawPixel(fitX + x, dstRow, logoSourcePixel(srcX, srcY));
    }
  }
}

void fillPixels(uint16_t* pixels, int width, int height, uint16_t color) {
  for (int i = 0; i < width * height; ++i) {
    pixels[i] = color;
  }
}

void fillRectPixels(uint16_t* pixels, int width, int height, int x, int y, int w, int h, uint16_t color) {
  const int x0 = std::max(0, x);
  const int y0 = std::max(0, y);
  const int x1 = std::min(width, x + w);
  const int y1 = std::min(height, y + h);
  for (int yy = y0; yy < y1; ++yy) {
    uint16_t* row = pixels + yy * width;
    for (int xx = x0; xx < x1; ++xx) {
      row[xx] = color;
    }
  }
}

void drawLogoFitHeightToPixels(uint16_t* pixels, int width, int height, int dstX, int dstY, int dstW, int dstH) {
  int fitX = 0;
  int fitY = 0;
  int fitW = 0;
  int fitH = 0;
  fitHeightRect(kLogoSrcW, kLogoSrcH, dstX, dstY, dstW, dstH, &fitX, &fitY, &fitW, &fitH);

  for (int y = 0; y < fitH; ++y) {
    const int dstRow = fitY + y;
    const int srcY = std::min(kLogoSrcH - 1, (y * kLogoSrcH) / fitH);
    uint16_t* row = pixels + dstRow * width;
    for (int x = 0; x < fitW; ++x) {
      const int srcX = std::min(kLogoSrcW - 1, (x * kLogoSrcW) / fitW);
      row[fitX + x] = logoSourcePixel(srcX, srcY);
    }
  }
}

void setModal(const String& text, uint16_t color, uint32_t now, uint32_t durationMs = kModalMs) {
  app.modalText = text;
  app.modalColor = color;
  app.modalUntilMs = now + durationMs;
  app.home.frameDirty = true;
}

void setScreenMode(ScreenMode nextScreen, uint32_t now);

String urlEncode(const String& value) {
  static const char* hex = "0123456789ABCDEF";
  String encoded;
  encoded.reserve(value.length() * 3);
  for (size_t i = 0; i < value.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    const bool safe = std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
    if (safe) {
      encoded += static_cast<char>(c);
    } else {
      encoded += '%';
      encoded += hex[(c >> 4) & 0x0F];
      encoded += hex[c & 0x0F];
    }
  }
  return encoded;
}

String apiBaseUrl() {
  return String("http://") + targetHost();
}

String extractErrors(DynamicJsonDocument& doc) {
  if (!doc.containsKey("errors")) {
    return "";
  }

  String text;
  JsonArray errors = doc["errors"].as<JsonArray>();
  for (JsonVariant value : errors) {
    if (!text.isEmpty()) {
      text += ", ";
    }
    text += value.as<const char*>();
  }
  return text;
}

String jsonValueToString(JsonVariantConst value) {
  if (value.is<const char*>()) {
    return String(value.as<const char*>());
  }
  if (value.is<String>()) {
    return value.as<String>();
  }
  if (value.is<long>()) {
    return String(value.as<long>());
  }
  if (value.is<int>()) {
    return String(value.as<int>());
  }
  if (value.is<bool>()) {
    return value.as<bool>() ? "Yes" : "No";
  }

  String text;
  serializeJson(value, text);
  return text;
}

String extractDigits(const String& value) {
  String digits;
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (std::isdigit(static_cast<unsigned char>(c))) {
      digits += c;
    }
  }
  return digits;
}

String cpuLabelFromValue(const String& rawValue) {
  const String trimmed = trimCopy(rawValue);
  const String digits = extractDigits(trimmed);
  for (size_t i = 0; i < app.cpuChoiceCount; ++i) {
    if (trimCopy(app.cpuWireOptions[i]) == trimmed || extractDigits(app.cpuWireOptions[i]) == digits ||
        trimCopy(app.cpuDisplayOptions[i]).equalsIgnoreCase(trimmed)) {
      return app.cpuDisplayOptions[i];
    }
  }
  if (!digits.isEmpty()) {
    return digits + " MHz";
  }
  return trimmed.isEmpty() ? "Unknown" : trimmed;
}

int cpuIndexFromValue(const String& rawValue) {
  const String trimmed = trimCopy(rawValue);
  const String digits = extractDigits(trimmed);
  for (size_t i = 0; i < app.cpuChoiceCount; ++i) {
    if (trimCopy(app.cpuWireOptions[i]) == trimmed || extractDigits(app.cpuWireOptions[i]) == digits ||
        trimCopy(app.cpuDisplayOptions[i]).equalsIgnoreCase(trimmed)) {
      return static_cast<int>(i);
    }
  }
  return 0;
}

void setFallbackCpuChoices() {
  static const char* fallback[] = {" 1", " 2", " 3", " 4", " 6", " 8", "10", "12",
                                   "14", "16", "20", "24", "32", "40", "48", "64"};
  app.cpuChoiceCount = std::min(kMaxCpuChoices, sizeof(fallback) / sizeof(fallback[0]));
  for (size_t i = 0; i < app.cpuChoiceCount; ++i) {
    app.cpuWireOptions[i] = fallback[i];
    app.cpuDisplayOptions[i] = trimCopy(fallback[i]) + " MHz";
  }
}

ApiResponse sendApiRequest(const char* method, const String& path, bool authenticated) {
  ApiResponse result;
  if (!hasTargetConfig()) {
    result.errors = "Target host missing";
    return result;
  }

  HTTPClient http;
  http.setTimeout(kHttpTimeoutMs);
  const String url = apiBaseUrl() + path;
  if (!http.begin(url)) {
    result.errors = "HTTP begin failed";
    return result;
  }

  if (authenticated && !targetPassword().isEmpty()) {
    http.addHeader("X-Password", targetPassword());
  }

  if (strcmp(method, "GET") == 0) {
    result.httpCode = http.GET();
  } else if (strcmp(method, "PUT") == 0) {
    result.httpCode = http.sendRequest("PUT", "");
  } else {
    http.end();
    result.errors = "Unsupported method";
    return result;
  }

  result.transportOk = result.httpCode > 0;
  if (result.transportOk) {
    result.body = http.getString();
    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, result.body) == DeserializationError::Ok) {
      result.jsonOk = true;
      result.errors = extractErrors(doc);
      result.apiOk = result.httpCode >= 200 && result.httpCode < 300 && result.errors.isEmpty();
    } else {
      result.apiOk = result.httpCode >= 200 && result.httpCode < 300;
    }
  } else {
    result.errors = http.errorToString(result.httpCode);
  }

  http.end();
  return result;
}

bool inspectCpuCategory(const String& category, String* itemOut, String* valueOut) {
  const ApiResponse response = sendApiRequest("GET", "/v1/configs/" + urlEncode(category), true);
  if (!response.apiOk) {
    return false;
  }

  DynamicJsonDocument doc(6144);
  if (deserializeJson(doc, response.body) != DeserializationError::Ok) {
    return false;
  }

  JsonVariant categoryObject = doc[category];
  if (categoryObject.isNull()) {
    for (JsonPair kv : doc.as<JsonObject>()) {
      if (String(kv.key().c_str()) != "errors" && kv.value().is<JsonObject>()) {
        categoryObject = kv.value();
        break;
      }
    }
  }
  if (categoryObject.isNull() || !categoryObject.is<JsonObject>()) {
    return false;
  }

  for (JsonPair kv : categoryObject.as<JsonObject>()) {
    const String key = kv.key().c_str();
    const String upper = key;
    if (upper.indexOf("CPU") >= 0 && upper.indexOf("Speed") >= 0) {
      *itemOut = key;
      *valueOut = jsonValueToString(kv.value());
      return true;
    }
  }
  return false;
}

bool refreshCpuChoices() {
  if (app.cpuCategory.isEmpty() || app.cpuItem.isEmpty()) {
    setFallbackCpuChoices();
    return false;
  }

  const ApiResponse response =
      sendApiRequest("GET", "/v1/configs/" + urlEncode(app.cpuCategory) + "/" + urlEncode(app.cpuItem), true);
  if (!response.apiOk) {
    setFallbackCpuChoices();
    return false;
  }

  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, response.body) != DeserializationError::Ok) {
    setFallbackCpuChoices();
    return false;
  }

  JsonVariant itemObject = doc[app.cpuCategory][app.cpuItem];
  if (itemObject.isNull()) {
    setFallbackCpuChoices();
    return false;
  }

  app.cpuChoiceCount = 0;
  JsonArray values = itemObject["values"].as<JsonArray>();
  for (JsonVariant value : values) {
    if (app.cpuChoiceCount >= kMaxCpuChoices) {
      break;
    }
    const String wire = jsonValueToString(value);
    const String digits = extractDigits(wire);
    app.cpuWireOptions[app.cpuChoiceCount] = wire;
    app.cpuDisplayOptions[app.cpuChoiceCount] = digits.isEmpty() ? trimCopy(wire) : digits + " MHz";
    app.cpuChoiceCount += 1;
  }

  if (app.cpuChoiceCount == 0) {
    setFallbackCpuChoices();
    return false;
  }

  app.currentCpuValue = cpuLabelFromValue(jsonValueToString(itemObject["current"]));
  return true;
}

bool resolveCpuPath(String* detailOut = nullptr) {
  if (app.cpuPathKnown) {
    if (app.cpuChoiceCount == 0) {
      refreshCpuChoices();
    }
    return true;
  }

  String item;
  String value;
  if (inspectCpuCategory("U64 Specific Settings", &item, &value)) {
    app.cpuCategory = "U64 Specific Settings";
    app.cpuItem = item;
    app.currentCpuValue = cpuLabelFromValue(value);
    app.cpuPathKnown = true;
    refreshCpuChoices();
    return true;
  }

  const ApiResponse listResponse = sendApiRequest("GET", "/v1/configs", true);
  if (!listResponse.apiOk) {
    if (detailOut != nullptr) {
      *detailOut = listResponse.errors.isEmpty() ? "Config list failed" : listResponse.errors;
    }
    return false;
  }

  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, listResponse.body) != DeserializationError::Ok) {
    if (detailOut != nullptr) {
      *detailOut = "Config list parse failed";
    }
    return false;
  }

  JsonArray categories = doc["categories"].as<JsonArray>();
  for (JsonVariant valueVariant : categories) {
    const String category = valueVariant.as<const char*>();
    if (inspectCpuCategory(category, &item, &value)) {
      app.cpuCategory = category;
      app.cpuItem = item;
      app.currentCpuValue = cpuLabelFromValue(value);
      app.cpuPathKnown = true;
      refreshCpuChoices();
      return true;
    }
  }

  if (detailOut != nullptr) {
    *detailOut = "CPU speed item not found";
  }
  return false;
}

void refreshCpuValue() {
  String detail;
  if (!resolveCpuPath(&detail)) {
    app.currentCpuValue = detail;
    return;
  }

  refreshCpuChoices();
}

void beginWiFi(uint32_t now) {
  if (!hasWiFiConfig()) {
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(wifiSsid().c_str(), wifiPassword().c_str());
  app.lastWiFiAttemptMs = now;
}

void serviceWiFi(uint32_t now) {
  if (!configReady()) {
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  if (app.lastWiFiAttemptMs == 0 || now - app.lastWiFiAttemptMs >= kWiFiRetryMs) {
    beginWiFi(now);
  }
}

void runConnectionTest(uint32_t now) {
  app.connection = {};
  app.connection.wifiConnected = WiFi.status() == WL_CONNECTED;

  if (!configReady()) {
    app.connection.detail = "Missing build config";
    setModal("CONFIG MISSING", rgb565(255, 170, 84), now, 1700);
    setScreenMode(ScreenMode::Status, now);
    return;
  }

  if (!app.connection.wifiConnected) {
    app.connection.detail = "WiFi not connected";
    beginWiFi(now);
    setModal("WIFI NOT READY", rgb565(255, 170, 84), now, 1700);
    setScreenMode(ScreenMode::Status, now);
    return;
  }

  const ApiResponse reach = sendApiRequest("GET", "/v1/version", false);
  app.connection.targetReachable = reach.transportOk;
  if (!reach.transportOk) {
    app.connection.detail = reach.errors.isEmpty() ? "Target unreachable" : reach.errors;
    setModal("TARGET OFFLINE", rgb565(255, 120, 96), now, 1700);
    setScreenMode(ScreenMode::Status, now);
    return;
  }

  const ApiResponse auth = sendApiRequest("GET", "/v1/version", true);
  app.connection.authOk = auth.apiOk;
  app.connection.detail = auth.apiOk ? "Reachable + auth ok" : (auth.errors.isEmpty() ? "Auth failed" : auth.errors);

  if (auth.apiOk) {
    refreshCpuValue();
    setModal("AUTH OK", rgb565(110, 230, 170), now, 1300);
  } else {
    setModal("AUTH FAILED", rgb565(255, 170, 84), now, 1700);
  }
  setScreenMode(ScreenMode::Status, now);
}

void performReset(uint32_t now) {
  if (WiFi.status() != WL_CONNECTED) {
    beginWiFi(now);
    setModal("NO WIFI", rgb565(255, 170, 84), now);
    return;
  }

  const ApiResponse response = sendApiRequest("PUT", "/v1/machine:reset", true);
  if (response.apiOk) {
    setModal("RESETTING...", rgb565(110, 230, 170), now, 1500);
  } else {
    const String text = response.errors.isEmpty() ? "RESET FAILED" : response.errors;
    setModal(text, rgb565(255, 120, 96), now, 1900);
  }
}

void performHardReset(uint32_t now) {
  if (WiFi.status() != WL_CONNECTED) {
    beginWiFi(now);
    setModal("NO WIFI", rgb565(255, 170, 84), now);
    return;
  }

  const ApiResponse response = sendApiRequest("PUT", "/v1/machine:reboot", true);
  if (response.apiOk) {
    setModal("HARD RESET", rgb565(255, 120, 96), now, 1600);
  } else {
    const String text = response.errors.isEmpty() ? "REBOOT FAILED" : response.errors;
    setModal(text, rgb565(255, 120, 96), now, 1900);
  }
}

void performMenuButton(uint32_t now) {
  if (WiFi.status() != WL_CONNECTED) {
    beginWiFi(now);
    setModal("NO WIFI", rgb565(255, 170, 84), now);
    return;
  }

  const ApiResponse response = sendApiRequest("PUT", "/v1/machine:menu_button", true);
  if (response.apiOk) {
    setModal("ULTIMATE MENU", rgb565(120, 220, 255), now, 1200);
  } else {
    const String text = response.errors.isEmpty() ? "MENU FAILED" : response.errors;
    setModal(text, rgb565(255, 120, 96), now, 1900);
  }
}

void setCpuSpeed(int cpuIndex, uint32_t now) {
  if (WiFi.status() != WL_CONNECTED) {
    beginWiFi(now);
    setModal("NO WIFI", rgb565(255, 170, 84), now);
    return;
  }

  String detail;
  if (!resolveCpuPath(&detail)) {
    setModal(detail, rgb565(255, 120, 96), now, 1900);
    return;
  }

  if (app.cpuChoiceCount == 0) {
    refreshCpuChoices();
  }
  const int clampedIndex = std::max(0, std::min(cpuIndex, static_cast<int>(app.cpuChoiceCount) - 1));
  const String displayValue = app.cpuDisplayOptions[clampedIndex];
  const String wireValue = app.cpuWireOptions[clampedIndex];
  const String path = "/v1/configs/" + urlEncode(app.cpuCategory) + "/" + urlEncode(app.cpuItem)
                    + "?value=" + urlEncode(wireValue);
  const ApiResponse response = sendApiRequest("PUT", path, true);
  if (response.apiOk) {
    refreshCpuValue();
    setModal(displayValue, rgb565(110, 230, 170), now, 1400);
  } else {
    const String text = response.errors.isEmpty() ? "CPU SET FAILED" : response.errors;
    setModal(text, rgb565(255, 120, 96), now, 1900);
  }
}

void drawLabelValue(int x, int y, const char* label, const String& value, uint16_t valueColor = TFT_WHITE) {
  canvas.setTextDatum(top_left);
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(rgb565(156, 190, 228));
  canvas.drawString(label, x, y);
  canvas.setTextColor(valueColor);
  canvas.drawString(value, x, y + 12);
}

void drawWrappedText(int x, int y, int width, const String& text, int maxLines, int lineHeight, uint16_t color) {
  canvas.setTextDatum(top_left);
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(color);

  String remaining = trimCopy(text);
  for (int line = 0; line < maxLines && !remaining.isEmpty(); ++line) {
    String current;
    int split = -1;
    for (int i = 0; i < remaining.length(); ++i) {
      const char c = remaining[i];
      if (c == ' ') {
        split = i;
      }

      const String candidate = remaining.substring(0, i + 1);
      if (canvas.textWidth(candidate) > width) {
        break;
      }
      current = candidate;
    }

    if (current.isEmpty()) {
      current = remaining;
    }

    if (canvas.textWidth(current) > width) {
      if (split > 0) {
        current = remaining.substring(0, split);
      } else {
        current = remaining.substring(0, std::min(static_cast<int>(remaining.length()), 12));
      }
    }

    current.trim();
    String next = remaining.substring(current.length());
    next.trim();

    if (line == maxLines - 1 && !next.isEmpty()) {
      while (!current.isEmpty() && canvas.textWidth(current + "...") > width) {
        current.remove(current.length() - 1);
        current.trim();
      }
      current += "...";
      next = "";
    }

    canvas.drawString(current, x, y + line * lineHeight);
    remaining = next;
  }
}

uint16_t samplePlainLogoPixel(int x, int y) {
  if (x < 0 || x >= canvas.width() || y < 0 || y >= canvas.height()) {
    return TFT_WHITE;
  }
  return plainLogoPixels[y * canvas.width() + x];
}

int wrapCoord(int value, int limit) {
  if (limit <= 0) {
    return 0;
  }
  value %= limit;
  if (value < 0) {
    value += limit;
  }
  return value;
}

void drawBoxedLogoInner() {
  const int innerX = homeInnerX();
  const int innerY = homeInnerY();
  const int innerW = homeInnerW();
  const int innerH = homeInnerH();
  for (int y = 0; y < innerH; ++y) {
    canvas.pushImage(innerX, innerY + y, innerW, 1, boxedLogoPixels + (innerY + y) * canvas.width() + innerX);
  }
}

void drawPlainLogoFrame() {
  const int width = canvas.width();
  const int height = canvas.height();
  for (int y = 0; y < height; ++y) {
    canvas.pushImage(0, y, width, 1, plainLogoPixels + y * width);
  }
}

void drawLogoDistortedRows(uint32_t tickMs, bool waterMode) {
  const float t = static_cast<float>(tickMs) * 0.001f;
  const int width = canvas.width();
  const int height = canvas.height();
  const float cx = width * 0.5f;

  for (int y = 0; y < height; ++y) {
    const float baseWave = sinf(y * 0.10f + t * (waterMode ? 8.2f : 3.2f));
    const float secondWave = sinf(y * 0.035f - t * (waterMode ? 4.9f : 2.1f));
    const float xOffset = waterMode ? (baseWave * 10.8f + secondWave * 5.8f) : (baseWave * 7.0f + secondWave * 9.0f);
    const float yOffset = waterMode ? (secondWave * 3.0f) : (baseWave * 3.0f);
    const float shimmer = waterMode ? std::max(0.0f, sinf(y * 0.07f + t * 10.8f)) : 0.0f;
    const int srcY = std::max(0, std::min(height - 1, static_cast<int>(y + yOffset)));

    for (int x = 0; x < width; ++x) {
      const float sampleX = cx + (static_cast<float>(x) - cx) + xOffset;
      uint16_t color = samplePlainLogoPixel(static_cast<int>(sampleX), srcY);
      if (waterMode) {
        color = blend565(color, rgb565(180, 240, 255), shimmer * 0.28f);
        logoRow[x] = color;
      } else {
        logoRow[x] = blend565(color, rgb565(82, 180, 255), 0.08f);
      }
    }
    canvas.pushImage(0, y, width, 1, logoRow);
  }
}

void drawLogoRotoZoom(uint32_t tickMs) {
  const float t = static_cast<float>(tickMs) * 0.001f;
  const int width = canvas.width();
  const int height = canvas.height();
  const float srcCx = width * 0.5f;
  const float srcCy = height * 0.5f;
  const float dstCx = width * 0.5f;
  const float dstCy = height * 0.5f;
  const float angle = t * 1.8f;
  const float zoom = 1.33f + 0.65f * sinf(t * 1.25f);
  const float cs = cosf(angle) / zoom;
  const float sn = sinf(angle) / zoom;

  for (int y = 0; y < height; ++y) {
    const float py = static_cast<float>(y) - dstCy;
    for (int x = 0; x < width; ++x) {
      const float px = static_cast<float>(x) - dstCx;
      const int srcX = wrapCoord(static_cast<int>(srcCx + px * cs - py * sn), width);
      const int srcY = wrapCoord(static_cast<int>(srcCy + px * sn + py * cs), height);
      logoRow[x] = samplePlainLogoPixel(srcX, srcY);
    }
    canvas.pushImage(0, y, width, 1, logoRow);
  }
}

void drawLogoBumpRipple(uint32_t tickMs) {
  const float t = static_cast<float>(tickMs) * 0.001f;
  const int width = canvas.width();
  const int height = canvas.height();
  const float cx = width * 0.5f;
  const float cy = height * 0.5f;
  const float lightX = -0.58f;
  const float lightY = -0.42f;
  const float maxRadius = sqrtf(static_cast<float>(width * width + height * height)) * 0.5f;
  const float travel = t * 118.0f;
  const float span = maxRadius * 2.0f;
  const float waveFreq = 0.17f;
  const float timeFreq = 15.8f;
  const float displacementScale = 24.0f;
  const float sourceX[3] = {cx, cx - 40.0f, cx + 34.0f};
  const float sourceY[3] = {cy, cy + 28.0f, cy - 24.0f};
  const float sourceWeight[3] = {1.0f, 0.46f, 0.38f};

  for (int y = 0; y < height; ++y) {
    const float fy = static_cast<float>(y);
    for (int x = 0; x < width; ++x) {
      const float fx = static_cast<float>(x);
      float gradX = 0.0f;
      float gradY = 0.0f;
      float waveMix = 0.0f;
      for (int i = 0; i < 3; ++i) {
        const float dx = fx - sourceX[i];
        const float dy = fy - sourceY[i];
        const float radius = sqrtf(dx * dx + dy * dy) + 0.0001f;
        float reflected = fmodf(radius + travel * (0.94f + 0.08f * i), span);
        float bounceDir = 1.0f;
        if (reflected > maxRadius) {
          reflected = span - reflected;
          bounceDir = -1.0f;
        }

        const float normR = std::min(reflected / maxRadius, 1.0f);
        const float damping = 1.0f - normR * 0.55f;
        const float wave = reflected * waveFreq - t * (timeFreq + i * 0.8f);
        const float slope = cosf(wave) * waveFreq * damping * bounceDir * sourceWeight[i];
        gradX += slope * (dx / radius);
        gradY += slope * (dy / radius);
        waveMix += sinf(wave) * damping * sourceWeight[i];
      }

      const int srcX = std::max(0, std::min(width - 1, static_cast<int>(fx - gradX * displacementScale)));
      const int srcY = std::max(0, std::min(height - 1, static_cast<int>(fy - gradY * displacementScale)));

      uint16_t color = samplePlainLogoPixel(srcX, srcY);

      const float shade = std::max(0.0f, (-gradX * lightX - gradY * lightY) * 0.85f);
      const float highlight = std::min(0.48f, shade * 0.52f);
      const float shadow = std::min(0.26f, std::max(0.0f, (gradX * lightX + gradY * lightY) * 0.36f));
      color = blend565(color, rgb565(215, 244, 255), highlight);
      color = blend565(color, rgb565(18, 44, 76), shadow);
      color = blend565(color, rgb565(120, 196, 255), std::min(0.24f, fabsf(waveMix) * 0.10f));
      logoRow[x] = color;
    }
    canvas.pushImage(0, y, width, 1, logoRow);
  }
}

uint32_t homeModeDuration(HomeMode mode) {
  switch (mode) {
    case HomeMode::Static:
      return kHomeStaticMs;
    case HomeMode::RotoZoom:
    case HomeMode::RippleBump:
      return kHomeLongEffectMs;
    default:
      return kHomeEffectMs;
  }
}

void drawHomeTransitionFlash(uint32_t phaseTimeMs, HomeMode mode) {
  if (phaseTimeMs >= 150) {
    return;
  }

  if (phaseTimeMs < 40) {
    canvas.fillScreen(mode == HomeMode::Static ? rgb565(255, 252, 244) : rgb565(222, 236, 255));
    return;
  }

  const uint16_t flashColor = mode == HomeMode::Static ? rgb565(255, 246, 214) : rgb565(196, 224, 255);
  const int spacing = phaseTimeMs < 100 ? 3 : 5;
  for (int y = 0; y < canvas.height(); y += spacing) {
    canvas.drawFastHLine(0, y, canvas.width(), flashColor);
  }
  canvas.drawRect(0, 0, canvas.width(), canvas.height(), flashColor);
}

void drawRasterBarsBorder(uint32_t tickMs) {
  const float t = static_cast<float>(tickMs) * 0.0014f;
  const uint16_t palette[] = {
      rgb565(255, 92, 164), rgb565(255, 190, 94), rgb565(132, 255, 184), rgb565(96, 196, 255)};
  const int innerX = homeInnerX();
  const int innerY = homeInnerY();
  const int innerW = homeInnerW();
  const int innerH = homeInnerH();

  canvas.fillScreen(rgb565(100, 128, 214));
  for (int y = 0; y < canvas.height(); ++y) {
    const int band = static_cast<int>(fmodf(y + t * 120.0f, 56.0f) / 14.0f) & 3;
    const uint16_t color = palette[band];
    if (y < innerY || y >= innerY + innerH) {
      canvas.drawFastHLine(0, y, canvas.width(), color);
    } else {
      canvas.drawFastHLine(0, y, innerX, color);
      canvas.drawFastHLine(innerX + innerW, y, canvas.width() - (innerX + innerW), color);
    }
  }

  canvas.fillRect(innerX, innerY, innerW, innerH, rgb565(53, 75, 121));
  canvas.drawRect(innerX, innerY, innerW, innerH, rgb565(132, 170, 255));
  drawBoxedLogoInner();
}

HomeMode nextHomeEffect(uint8_t effectIndex) {
  switch (effectIndex % 5u) {
    case 0:
      return HomeMode::Water;
    case 1:
      return HomeMode::RotoZoom;
    case 2:
      return HomeMode::SineWave;
    case 3:
      return HomeMode::RippleBump;
    default:
      return HomeMode::RasterBars;
  }
}

void enterHomeMode(HomeMode mode, uint32_t now) {
  app.home.mode = mode;
  app.home.startedAtMs = now;
  app.home.frameDirty = true;
}

void updateHomeDemo(uint32_t now) {
  if (app.home.startedAtMs == 0) {
    enterHomeMode(HomeMode::Static, now);
    return;
  }

  const uint32_t durationMs = homeModeDuration(app.home.mode);
  if (now - app.home.startedAtMs < durationMs) {
    return;
  }

  if (app.home.mode == HomeMode::Static) {
    enterHomeMode(nextHomeEffect(app.home.nextEffectIndex), now);
  } else {
    app.home.nextEffectIndex = static_cast<uint8_t>((app.home.nextEffectIndex + 1) % 5u);
    enterHomeMode(HomeMode::Static, now);
  }
}

void drawHomeDemo(uint32_t now) {
  const uint32_t phaseTimeMs = now - app.home.startedAtMs;
  switch (app.home.mode) {
    case HomeMode::Static:
      drawPlainLogoFrame();
      break;
    case HomeMode::Water:
      drawLogoDistortedRows(phaseTimeMs, true);
      break;
    case HomeMode::RotoZoom:
      drawLogoRotoZoom(phaseTimeMs);
      break;
    case HomeMode::SineWave:
      drawLogoDistortedRows(phaseTimeMs, false);
      break;
    case HomeMode::RippleBump:
      drawLogoBumpRipple(phaseTimeMs);
      break;
    case HomeMode::RasterBars:
      drawRasterBarsBorder(phaseTimeMs);
      break;
  }
  drawHomeTransitionFlash(phaseTimeMs, app.home.mode);
}

void drawMenuTitle(const char* title) {
  canvas.setTextDatum(top_center);
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(rgb565(196, 224, 255));
  canvas.drawString(title, canvas.width() / 2, 8);
}

void drawModal(uint32_t now) {
  if (app.modalText.isEmpty() || now > app.modalUntilMs) {
    return;
  }

  const int boxW = 206;
  const int boxH = 72;
  const int x = (canvas.width() - boxW) / 2;
  const int y = (canvas.height() - boxH) / 2;
  const uint16_t shell = rgb565(6, 10, 18);
  const uint16_t inner = rgb565(24, 32, 54);

  canvas.fillRoundRect(x, y, boxW, boxH, 12, shell);
  canvas.drawRoundRect(x, y, boxW, boxH, 12, app.modalColor);
  canvas.fillRoundRect(x + 6, y + 6, boxW - 12, boxH - 12, 10, inner);
  canvas.fillRoundRect(x + 12, y + 12, boxW - 24, 6, 3, app.modalColor);

  canvas.setTextDatum(top_center);
  canvas.setFont(&fonts::Font4);
  canvas.setTextColor(app.modalColor);
  const bool longText = canvas.textWidth(app.modalText) > (boxW - 24);
  if (longText) {
    canvas.setFont(&fonts::Font2);
    drawWrappedText(x + 12, y + 19, boxW - 24, app.modalText, 3, 14, app.modalColor);
  } else {
    canvas.drawString(app.modalText, canvas.width() / 2, y + 24);
  }
}

void drawHome() {
  drawPlainLogoFrame();
}

void drawMenu() {
  drawMenuTitle("MENU");
  for (size_t index = 0; index < kMenuCount; ++index) {
    const int y = 26 + static_cast<int>(index) * 24;
    const bool selected = static_cast<int>(index) == app.menuIndex;
    const uint16_t fill = selected ? rgb565(58, 102, 184) : rgb565(18, 30, 52);
    const uint16_t border = selected ? rgb565(184, 228, 255) : rgb565(74, 108, 148);
    const uint16_t text = selected ? TFT_WHITE : rgb565(212, 226, 248);

    canvas.fillRoundRect(12, y, 216, 20, 8, fill);
    canvas.drawRoundRect(12, y, 216, 20, 8, border);
    canvas.setTextDatum(middle_center);
    canvas.setFont(&fonts::Font4);
    canvas.setTextColor(text);
    canvas.drawString(kMenuItems[index], canvas.width() / 2, y + 10);
  }
}

void drawCpuMenu() {
  drawMenuTitle("CPU SPEED");
  drawLabelValue(12, 20, "Current", app.currentCpuValue, rgb565(110, 230, 170));

  const int choiceCount = static_cast<int>(app.cpuChoiceCount == 0 ? 1 : app.cpuChoiceCount);
  const int start = std::max(0, std::min(app.cpuIndex - 1, choiceCount - 3));
  const int end = std::min(choiceCount, start + 3);
  for (int index = start; index < end; ++index) {
    const int y = 52 + (index - start) * 24;
    const bool selected = index == app.cpuIndex;
    const uint16_t fill = selected ? rgb565(48, 94, 164) : rgb565(16, 25, 42);
    const uint16_t border = selected ? rgb565(122, 228, 174) : rgb565(66, 96, 132);
    canvas.fillRoundRect(18, y, 204, 20, 8, fill);
    canvas.drawRoundRect(18, y, 204, 20, 8, border);
    canvas.setTextDatum(middle_center);
    canvas.setFont(&fonts::Font4);
    canvas.setTextColor(selected ? rgb565(220, 255, 232) : TFT_WHITE);
    canvas.drawString(app.cpuChoiceCount == 0 ? "?" : app.cpuDisplayOptions[index], canvas.width() / 2, y + 10);
  }
}

void drawStatus() {
  drawMenuTitle("STATUS");
  const bool wifiOk = WiFi.status() == WL_CONNECTED;

  drawLabelValue(12, 20, "WiFi", wifiOk ? "Connected" : "Disconnected",
                 wifiOk ? rgb565(110, 230, 170) : rgb565(255, 170, 84));
  drawLabelValue(124, 20, "Target",
                 app.connection.targetReachable ? "Reachable" : "Not reached",
                 app.connection.targetReachable ? rgb565(110, 230, 170) : rgb565(255, 170, 84));
  drawLabelValue(12, 56, "Auth", app.connection.authOk ? "OK" : "Not verified",
                 app.connection.authOk ? rgb565(110, 230, 170) : rgb565(255, 170, 84));
  drawLabelValue(124, 56, "CPU", app.currentCpuValue, TFT_WHITE);
  drawLabelValue(12, 92, "Host", targetHost(), rgb565(208, 224, 248));

  canvas.fillRoundRect(118, 88, 110, 38, 8, rgb565(16, 24, 40));
  canvas.drawRoundRect(118, 88, 110, 38, 8, rgb565(74, 108, 148));
  drawWrappedText(126, 96, 94, app.connection.detail, 2, 12, rgb565(182, 198, 220));
}

void drawAppUi() {
  canvas.fillScreen(rgb565(8, 14, 30));
  switch (app.screen) {
    case ScreenMode::Home:
      break;
    case ScreenMode::Menu:
      drawMenu();
      break;
    case ScreenMode::CpuMenu:
      drawCpuMenu();
      break;
    case ScreenMode::Status:
      drawStatus();
      break;
  }
}

void setScreenMode(ScreenMode nextScreen, uint32_t now) {
  if (app.screen == nextScreen) {
    return;
  }

  if (app.screen == ScreenMode::Home && nextScreen != ScreenMode::Home) {
    app.home.pausedAtMs = now;
  } else if (app.screen != ScreenMode::Home && nextScreen == ScreenMode::Home) {
    if (app.home.startedAtMs == 0) {
      enterHomeMode(HomeMode::Static, now);
    } else if (app.home.pausedAtMs != 0) {
      app.home.startedAtMs += now - app.home.pausedAtMs;
      app.home.pausedAtMs = 0;
    }
  }

  app.screen = nextScreen;
  app.home.frameDirty = true;
}

bool render(uint32_t now) {
  const bool modalVisible = !app.modalText.isEmpty() && now <= app.modalUntilMs;

  if (app.screen == ScreenMode::Home && app.home.mode == HomeMode::Static) {
    const bool needsRedraw = app.home.frameDirty || modalVisible != app.lastModalVisible;
    if (!needsRedraw) {
      return false;
    }
    drawPlainLogoFrame();
  } else if (app.screen == ScreenMode::Home) {
    drawHomeDemo(now);
  } else {
    drawAppUi();
  }

  if (modalVisible) {
    drawModal(now);
  }
  canvas.pushSprite(0, 0);
  app.lastModalVisible = modalVisible;
  app.home.frameDirty = false;
  return true;
}

void handleMenuSelect(uint32_t now) {
  switch (app.menuIndex) {
    case 0:
      setScreenMode(ScreenMode::Home, now);
      performReset(now);
      break;
    case 1:
      refreshCpuValue();
      app.cpuIndex = cpuIndexFromValue(app.currentCpuValue);
      setScreenMode(ScreenMode::CpuMenu, now);
      break;
    case 2:
      runConnectionTest(now);
      break;
    case 3:
      setScreenMode(ScreenMode::Status, now);
      break;
  }
}

}  // namespace

void setup() {
  auto cfg = M5.config();
  cfg.clear_display = true;
  M5.begin(cfg);

  Serial.begin(115200);
  M5.Display.setRotation(3);
  canvas.setColorDepth(16);
  canvas.createSprite(M5.Display.width(), M5.Display.height());
  canvas.setSwapBytes(true);

  const size_t frameBytes = static_cast<size_t>(M5.Display.width()) * static_cast<size_t>(M5.Display.height()) * sizeof(uint16_t);
  plainLogoPixels = static_cast<uint16_t*>(ps_malloc(frameBytes));
  boxedLogoPixels = static_cast<uint16_t*>(ps_malloc(frameBytes));
  if (plainLogoPixels == nullptr || boxedLogoPixels == nullptr) {
    Serial.println("logo cache alloc failed");
    while (true) {
      delay(1000);
    }
  }

  fillPixels(plainLogoPixels, M5.Display.width(), M5.Display.height(), TFT_WHITE);
  drawLogoFitHeightToPixels(plainLogoPixels, M5.Display.width(), M5.Display.height(), 0, 0, M5.Display.width(),
                            M5.Display.height());

  fillPixels(boxedLogoPixels, M5.Display.width(), M5.Display.height(), TFT_WHITE);
  fillRectPixels(boxedLogoPixels, M5.Display.width(), M5.Display.height(), homeInnerX(), homeInnerY(), homeInnerW(),
                 homeInnerH(), TFT_WHITE);
  drawLogoFitHeightToPixels(boxedLogoPixels, M5.Display.width(), M5.Display.height(), homeInnerX(), homeInnerY(),
                            homeInnerW(), homeInnerH());

  const uint16_t leftSample = plainLogoPixels[(M5.Display.height() / 2) * M5.Display.width() + 10];
  const uint16_t blueSample =
      plainLogoPixels[(M5.Display.height() / 2) * M5.Display.width() + (M5.Display.width() / 2 - 28)];
  const uint16_t redSample =
      plainLogoPixels[(M5.Display.height() / 2 + 8) * M5.Display.width() + (M5.Display.width() / 2 + 28)];
  Serial.printf("logo cache samples left=%04X blue=%04X red=%04X\n", leftSample, blueSample, redSample);

  setFallbackCpuChoices();

  app.configReady = configReady();
  beginWiFi(millis());
  if (!app.configReady) {
    setModal("SET .env + REBUILD", rgb565(255, 170, 84), millis(), 2400);
  }
}

void loop() {
  static uint32_t nextFrameMs = 0;

  M5.update();
  const uint32_t now = millis();
  serviceWiFi(now);
  if (app.screen == ScreenMode::Home) {
    updateHomeDemo(now);
  }

  if (M5.BtnA.wasPressed()) {
    if (app.screen == ScreenMode::Home) {
      if (app.pendingSoftReset && now - app.pendingSoftResetAtMs <= kDoublePressMs) {
        app.pendingSoftReset = false;
        performHardReset(now);
      } else {
        app.pendingSoftReset = true;
        app.pendingSoftResetAtMs = now;
      }
    } else {
      setScreenMode((app.screen == ScreenMode::CpuMenu) ? ScreenMode::Menu : ScreenMode::Home, now);
    }
  }

  if (M5.BtnB.wasPressed()) {
    switch (app.screen) {
      case ScreenMode::Home:
        app.pendingSoftReset = false;
        performMenuButton(now);
        break;
      case ScreenMode::Menu:
        app.menuIndex = (app.menuIndex + 1) % static_cast<int>(kMenuCount);
        break;
      case ScreenMode::CpuMenu:
        app.cpuIndex = (app.cpuIndex + 1) % static_cast<int>(std::max<size_t>(1, app.cpuChoiceCount));
        break;
      case ScreenMode::Status:
        setScreenMode(ScreenMode::Menu, now);
        break;
    }
  }

  if (M5.BtnPWR.wasPressed()) {
    switch (app.screen) {
      case ScreenMode::Home:
        app.pendingSoftReset = false;
        setScreenMode(ScreenMode::Menu, now);
        break;
      case ScreenMode::Menu:
        handleMenuSelect(now);
        break;
      case ScreenMode::CpuMenu:
        setCpuSpeed(app.cpuIndex, now);
        break;
      case ScreenMode::Status:
        runConnectionTest(now);
        break;
    }
  }

  if (app.pendingSoftReset && now - app.pendingSoftResetAtMs > kDoublePressMs) {
    app.pendingSoftReset = false;
    performReset(now);
  }

  if (WiFi.status() == WL_CONNECTED && app.currentCpuValue == "Unknown" && app.screen == ScreenMode::Home) {
    refreshCpuValue();
  }

  if (nextFrameMs == 0 || static_cast<int32_t>(now - nextFrameMs) >= 0) {
    render(now);
    nextFrameMs = now + kFrameMs;
  }

  delay(1);
}
