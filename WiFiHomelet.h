#pragma once

#include <Ticker.h>
#include <Updater.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ESP8266mDNS.h>
#include <ESP8266WiFi.h>
#include <mDNSResolver.h>
#include <ESP8266WebServer.h>
#include <AsyncHTTPRequest_Generic.h>

class WiFiHomelet {

public:
    using WebServer = ESP8266WebServer;

private:
    WiFiEventHandler handlers[3];
    WebServer web;
    File file;
    WiFiUDP udp;
    mDNSResolver::Resolver resolver;
    Ticker ticker;

public:
    WiFiHomelet(): resolver(udp) {
#ifdef DEBUG
        Serial.begin(115200);
#endif
        LittleFS.begin();
    }

    void begin(const char* name) {
        WiFi.mode(WIFI_AP_STA);
        WiFi.persistent(true);
        WiFi.setAutoConnect(true);
        WiFi.setAutoReconnect(true);
        WiFi.hostname(name);
        handlers[0] = WiFi.onStationModeConnected(
            [](const WiFiEventStationModeConnected&) {
                WiFi.enableAP(false);
            });
        handlers[1] = WiFi.onStationModeDisconnected(
            [](const WiFiEventStationModeDisconnected&) {
                WiFi.enableAP(true);
            });
        handlers[2] = WiFi.onStationModeGotIP(
            [&](const WiFiEventStationModeGotIP& event) {
#ifdef DEBUG
                Serial.printf("Got IP: %s\n", event.ip.toString().c_str());
#endif
                MDNS.begin(WiFi.hostname(), event.ip);
                MDNS.addService("http", "tcp", 80);
                resolver.setLocalIP(event.ip);
            });
        IPAddress ap_ip(192, 168, 1, 1);
        IPAddress net_mask(255, 255, 255, 0);
        WiFi.softAPConfig(ap_ip, ap_ip, net_mask);
        WiFi.softAP(name);
        WiFi.begin();
        web.begin();
        onRequest("/wifi", [&](WebServer& web, JsonDocument& json) -> bool {
            if(!web.hasArg("ssid")) {
                json["code"] = -1;
                json["error"] = "<ssid> is mandatory, <passphrase> is optional";
                return true;
            }
            const String& ssid = web.arg("ssid");
            const String& passphrase = web.hasArg("passphrase") ?
                web.arg("passphrase") : emptyString;
            json["code"] = 0;
            json["ssid"] = ssid;
            json["passphrase"] = passphrase;
            ticker.once(3, [=](){ WiFi.begin(ssid, passphrase); });
            return true;
        });
        onRequest("/info", [&](WebServer&, JsonDocument& json) -> bool {
            json["code"] = 0;
            json["name"] = WiFi.hostname();
            json["free_heap"] = ESP.getFreeHeap();
            json["free_stack"] = ESP.getFreeContStack();
            json["free_sketch"] = ESP.getFreeSketchSpace();
            json["cpu_freq"] = ESP.getCpuFreqMHz();
            json["flash_size"] = ESP.getFlashChipRealSize();
            fs::FSInfo info;
            LittleFS.info(info);
            json["total_fs"] = info.totalBytes;
            json["free_fs"] = info.totalBytes - info.usedBytes;
            json["mac"] = WiFi.macAddress();
            bool is_connected = WiFi.isConnected();
            json["connected"] = is_connected;
            if(is_connected) {
                json["ssid"] = WiFi.SSID();
                json["channel"] = WiFi.channel();
                json["rssi"] = WiFi.RSSI();
                json["ip"] = WiFi.localIP().toString();
            }
            return true;
        }, 512);
        onRequest("/restart", [&](WebServer&, JsonDocument& json) -> bool {
            json["code"] = 0;
            ticker.once(3, [](){ ESP.restart(); });
            return true;
        });
        onRequest("/delete", [](WebServer& web, JsonDocument& json) -> bool {
            if(!web.hasArg("file")) {
                json["code"] = -1;
                json["error"] = "<file> = */[file name] is mandatory";
                return true;
            }
            const String& file = web.arg("file");
            if(file.equals("*")) {
                if(!LittleFS.format()) {
                    json["code"] = -1;
                    json["error"] = "failed to format filesystem";
                    return true;
                }
            }
            else if(!LittleFS.remove(file)) {
                json["code"] = -1;
                json["error"] = "failed to delete " + file;
                return true;
            }
            json["code"] = 0;
            json["file"] = file;
            return true;
        });
        web.on("/upload", HTTP_ANY,
            [&]() {
                static const char* PROGMEM html = 
                    "Upload a file to filesytem<br>"
                    "<form method='POST' action='/upload' enctype='multipart/form-data'>"
                        "<input type='file' name='file'>"
                        "<input type='submit' value='upload'>"
                    "</form>";
                web.send(200, "text/html", html);
            },
            [&]() {
                int code = 1;
                String msg;
                HTTPUpload& upload = web.upload();
                if(upload.status == UPLOAD_FILE_START) {
                    fs::FSInfo info;
                    LittleFS.info(info);
                    if(info.usedBytes + upload.contentLength > info.totalBytes) {
                        code = -1;
                        msg = "no enough space for " + upload.filename;
                    }
                    else if(!(file = LittleFS.open("/" + upload.filename, "w"))) {
                        code = -1;
                        msg = "failed to open " + upload.filename;
                    }
                }
                else if(upload.status == UPLOAD_FILE_WRITE) {
                    if(file.write(upload.buf, upload.currentSize) != upload.currentSize) {
                        code = -1;
                        msg = "failed to write to " + upload.filename;
                    }
#ifdef DEBUG
                    Serial.printf("so far uploaded %lu bytes\n", upload.totalSize);
#endif
                }
                else if(upload.status == UPLOAD_FILE_END) {
                    code = 0;
                    msg = file.size();
                    file.close();
                }
                else {
                    code = -1;
                    msg = "aborted to upload";
                }
                if(code == 0) {
                    web.send(200, "application/json", "{code: 0, size: " + msg + "}");
                }
                else if(code == -1) {
                    web.send(200, "application/json", "{code: -1, error: '" + msg + "'}");
                }
            }
        );
        web.on("/ota", HTTP_ANY,
            [&]() {
                static const char* PROGMEM html = 
                    "Upload a compiled binary file for OTA<br>"
                    "<form method='POST' action='/ota' enctype='multipart/form-data'>"
                        "<input type='file' name='file' accept='.bin'>"
                        "<input type='submit' value='upload'>"
                    "</form>";
                web.send(200, "text/html", html);
            },
            [&]() {
                int code = 1;
                String msg;
                HTTPUpload& upload = web.upload();
                if(upload.status == UPLOAD_FILE_START) {
                    if(!Update.begin(upload.contentLength, U_FLASH)){
                        code = -1;
                        msg = upload.filename + " is too large for OTA";
                    }
                }
                else if(upload.status == UPLOAD_FILE_WRITE) {
                    if(Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                        code = -1;
                        msg = "failed to write to flash";
                    }
#ifdef DEBUG
                    Serial.printf("so far uploaded %lu bytes\n", upload.totalSize);
#endif
                }
                else if(upload.status == UPLOAD_FILE_END) {
                    if(Update.end(true)) {
                        code = 0;
                        msg = upload.totalSize;
                    }
                    else {
                        code = -1;
                        msg = "failed to write the config to eboot";
                    }
                }
                else {
                    Update.end(false);
                    code = -1;
                    msg = "aborted to upload";
                }
                if(code == 0) {
                    web.send(200, "application/json", "{code: 0, size: " + msg + "}");
                }
                else if(code == -1) {
                    web.send(200, "application/json", "{code: -1, error: '" + msg + "'}");
                }
            }
        );
    }

    void onRequest(const char* URI, std::function<bool(WebServer&, JsonDocument&)> handler,
            size_t json_size = 256) {
        web.on(URI, [&, handler, json_size]() {
            DynamicJsonDocument json(json_size);
            if(!handler(web, json)) {
                return;
            }
            size_t buffer_size = json_size * 2;
            char* buffer = new char [buffer_size];
            size_t len = serializeJson(json, buffer, buffer_size);
            web.send(200, "application/json", buffer, len);
            delete[] buffer;
        });
    }

    bool httpGet(const char* URL, std::function<void(const String&)> callback = nullptr) {
        String url(URL);
        if(!url.startsWith("http://")) {
            return false;
        }
        int start = 7;
        int end = url.indexOf('/', start + 1);
        String host = end < 0 ? url.substring(start) : url.substring(start, end);
        if(host.endsWith(".local")) {
            IPAddress ip = resolver.search(host.c_str());
            if(ip == INADDR_NONE) {
                return false;
            }
#ifdef DEBUG
            Serial.printf("mDNS: %s => %s\n", host.c_str(), ip.toString().c_str());
#endif
            url = "http://" + ip.toString() + (end < 0 ? emptyString : url.substring(end));
        }
#ifdef DEBUG
        Serial.printf("request to %s\n", url.c_str());
#endif
        auto* request = new AsyncHTTPRequest;
        request->onReadyStateChange(
            [=](void*, AsyncHTTPRequest* request, int state) {
#ifdef DEBUG
                Serial.printf("HTTP state: %d\n", state);
#endif
                if(state == readyStateOpened) {
                    request->send();
                }
                else if(state == readyStateDone) {
#ifdef DEBUG
                    Serial.printf("HTTP code: %d, text: %s\n",
                        request->responseHTTPcode(), request->responseText().c_str());
#endif
                    if(callback) {
                        callback(request->responseText());
                    }
                    delete request;
                }
            });
        request->open("GET", url.c_str());
        return true;
    }

    void tick() {
        MDNS.update();
        web.handleClient();
        resolver.loop();
    }

};
