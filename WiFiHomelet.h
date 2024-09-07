#pragma once

#include <Updater.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ESP8266mDNS.h>
#include <ESP8266WiFi.h>
#include <mDNSResolver.h>
#include <ESP8266WebServer.h>
#include <AsyncHTTPRequest_Generic.h>

#include <queue>

class WiFiHomelet {

public:
    using WebServer = ESP8266WebServer;

private:
    static constexpr int NREQUESTS = 3;

    WiFiEventHandler wifi_handlers[3];
    WebServer web;
    WiFiUDP udp;
    mDNSResolver::Resolver resolver;
    std::queue<String> urls;

    struct Epilogue {
        std::function<void(void)> func;
        uint32_t start;
        uint32_t delay;

        void set(std::function<void(void)> func, uint32_t delay) {
            this->func = func;
            this->delay = delay;
            start = millis();
        }

        void operator()() {
            if(func && millis() - start >= delay) {
                func();
                func = nullptr;
            }
        }
    }
    epilogue;

    struct Request {
        static constexpr uint32_t COOLING_TIME = 3000;

        AsyncHTTPRequest request;
        uint32_t start;

        Request() {
            request.onReadyStateChange([](void* self, AsyncHTTPRequest* request, int state) {
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
                    reinterpret_cast<Request*>(self)->start = std::max(2UL, millis());
                }
            },
            this);
            start = 0;
        }

        void tick() {
            if(start > 1 && millis() - start >= COOLING_TIME) {
                start = 0;
            }
        }

        bool get(const char* url) {
            if(start != 0) {
                return false;
            }
            start = 1;
            request.open("GET", url);
#ifdef DEBUG
            Serial.printf("request to %s\n", url);
#endif
            return true;
        }
    }
    requests[NREQUESTS];

public:
    WiFiHomelet(): resolver(udp) {}

    void begin(const char* name) {
        WiFi.mode(WIFI_AP_STA);
        WiFi.persistent(true);
        WiFi.setAutoConnect(true);
        WiFi.setAutoReconnect(true);
        WiFi.hostname(name);
        wifi_handlers[0] = WiFi.onStationModeConnected(
            [](const WiFiEventStationModeConnected&) {
                WiFi.enableAP(false);
            });
        wifi_handlers[1] = WiFi.onStationModeDisconnected(
            [](const WiFiEventStationModeDisconnected&) {
                WiFi.enableAP(true);
            });
        wifi_handlers[2] = WiFi.onStationModeGotIP(
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
        LittleFS.begin();
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
            epilogue.set([=](){ WiFi.begin(ssid, passphrase); }, 3000);
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
            epilogue.set([](){ ESP.restart(); }, 5000);
            json["code"] = 0;
            return true;
        });
        onRequest("/delete", [](WebServer& web, JsonDocument& json) -> bool {
            if(!web.hasArg("file")) {
                json["code"] = -1;
                json["error"] = "<file> = * / [file name] is mandatory";
                return true;
            }
            const String& file = web.arg("file");
            if(file.equals("*")) {
                Dir dir = LittleFS.openDir("/");
                while(dir.next()) {
                    if(!LittleFS.remove(dir.fileName())) {
                        json["code"] = -1;
                        json["error"] = "failed to delete " + dir.fileName();
                        return true;
                    }
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
                static File file;
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

    bool request(const char* URL) {
        String url(URL);
        if(!url.startsWith("http://")) {
            return false;
        }
        int start = 7;
        int end = url.indexOf('/', start + 1);
        String host = end < 0 ? url.substring(start) : url.substring(start, end);
        if(host.endsWith(".local")) {
            IPAddress ip = resolver.search(host.c_str());
#ifdef DEBUG
            Serial.printf("mDNS: %s => %s\n", host.c_str(), ip.toString().c_str());
#endif
            if(ip == INADDR_NONE) {
                return false;
            }
            url = "http://" + ip.toString() + (end < 0 ? emptyString : url.substring(end));
        }
        urls.push(url);
        return true;
    }

    void tick() {
        MDNS.update();
        web.handleClient();
        resolver.loop();
        epilogue();
        for(int i = 0; i < NREQUESTS; i++) {
            requests[i].tick();
        }
        if(!urls.empty()) {
            const char* url = urls.front().c_str();
            for(int i = 0; i < NREQUESTS; i++) {
                if(requests[i].get(url)) {
                    urls.pop();
                    break;
                }
            }
        }
    }

};
