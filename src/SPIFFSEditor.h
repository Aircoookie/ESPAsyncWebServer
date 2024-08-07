#ifndef SPIFFSEditor_H_
#define SPIFFSEditor_H_
#include <ESPAsyncWebServer.h>
#ifdef ESP8266
#include <LittleFS.h>
#endif

//this indicates that this implementation will not serve the wsec.json file from FS
#define SPIFFS_EDITOR_AIRCOOOKIE

class SPIFFSEditor: public AsyncWebHandler {
  private:
    fs::FS _fs;
    String _username;
    String _password; 
    bool _authenticated;
    uint32_t _startTime;
  public:
    SPIFFSEditor(const fs::FS& fs, const String& username=String(), const String& password=String());
#ifdef ESP8266
    // Alternate constructor, defaults to LittleFS
    SPIFFSEditor(const String& username=String(), const String& password=String(), const fs::FS& fs=LittleFS);
#endif    

    virtual bool canHandle(AsyncWebServerRequest *request) override final;
    virtual void handleRequest(AsyncWebServerRequest *request) override final;
    virtual void handleUpload(AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool final) override final;
    virtual bool isRequestHandlerTrivial() override final {return false;}
};

#endif
