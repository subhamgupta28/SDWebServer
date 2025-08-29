#include "SDWebServer.h"

SDWebServer::SDWebServer(int port)
    : card(NULL), ownsServer(true)
{
    server = new AsyncWebServer(port); // create internally
}

SDWebServer::SDWebServer(AsyncWebServer &existingServer)
    : card(NULL), ownsServer(false)
{
    server = &existingServer; // just take pointer, don’t own
}

SDWebServer::~SDWebServer()
{
    if (ownsServer && server)
    {
        delete server; // free only if we created it
    }
}

bool SDWebServer::begin()
{

    // initSD();
    initRoutes();

    if (ownsServer)
        server->begin();
    return card != NULL;
}

void SDWebServer::initSD()
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 10,
        .allocation_unit_size = 512,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
    slot_config.clk = (gpio_num_t)4;
    slot_config.cmd = (gpio_num_t)3;
    slot_config.d0 = (gpio_num_t)5;
    slot_config.d1 = (gpio_num_t)6;
    slot_config.d2 = (gpio_num_t)42;
    slot_config.d3 = (gpio_num_t)2;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_WEB, &host, &slot_config, &mount_config, &card);
    if (ret == ESP_OK && card != NULL)
    {
        Serial.println("SD card mounted.");
        sdmmc_card_print_info(stdout, card);
    }
    else
    {
        Serial.printf("Failed to mount SD card (err: %d)\n", ret);
    }
}
// -------------------- Helpers --------------------
static inline String joinPath(const String &base, const String &name)
{
    if (base.endsWith("/"))
        return base + name;
    return base + "/" + name;
}

static inline String vfsFromFat(const String &fatPath)
{
    // Convert "0:/foo/bar" -> "/sdcard/foo/bar"
    if (fatPath.startsWith("0:/"))
    {
        return SD_MOUNT_WEB + fatPath.substring(2); // replace "0:" with SD_MOUNT_WEB
    }
    return SD_MOUNT_WEB; // fallback
}

String SDWebServer::listFilesRecursive(const String &fatDir, const String &vfsDir, uint8_t levels)
{
    String json = "[";
    bool first = true;

    // Use stdio VFS API to iterate directories via opendir/readdir if desired,
    // but here we rely on FatFs through VFS layer using dirent from 'ff.h' via f_opendir/f_readdir:
    FRESULT res;
    FILINFO fno;
    FF_DIR dir;

    res = f_opendir(&dir, fatDir.c_str());
    if (res == FR_OK)
    {
        for (;;)
        {
            res = f_readdir(&dir, &fno);
            if (res != FR_OK || fno.fname[0] == 0)
                break;

            String name = fno.fname;
            String childFat = joinPath(fatDir, name);
            String childVfs = joinPath(vfsDir, name);

            if (!first)
                json += ",";
            json += "{";
            json += "\"name\":\"" + name + "\",";
            json += "\"path\":\"" + childVfs + "\",";

            if (fno.fattrib & AM_DIR)
            {
                json += "\"type\":\"dir\"";
                if (levels > 0)
                {
                    json += ",\"children\":";
                    json += listFilesRecursive(childFat, childVfs, levels - 1);
                }
            }
            else
            {
                json += "\"type\":\"file\",\"size\":" + String((uint32_t)fno.fsize);
            }
            json += "}";
            first = false;
        }
        f_closedir(&dir);
    }

    json += "]";
    return json;
}
bool SDWebServer::deleteRecursive(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0)
    {
        return false; // not found
    }

    if (S_ISDIR(st.st_mode))
    {
        DIR *dir = opendir(path);
        if (!dir)
            return false;

        struct dirent *de;
        while ((de = readdir(dir)) != NULL)
        {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;

            String childPath = String(path) + "/" + de->d_name;
            if (!deleteRecursive(childPath.c_str()))
            {
                closedir(dir);
                return false;
            }
        }
        closedir(dir);
        return (rmdir(path) == 0);
    }
    else
    {
        return (remove(path) == 0);
    }
}

bool SDWebServer::deleteRecursiveOld(const char *path)
{
    FILINFO fno;
    FRESULT res;
    FF_DIR dir;

    res = f_opendir(&dir, path);
    if (res == FR_OK)
    {
        while (true)
        {
            res = f_readdir(&dir, &fno);
            if (res != FR_OK || fno.fname[0] == 0)
                break;

            String filePath = String(path) + "/" + fno.fname;
            if (fno.fattrib & AM_DIR)
                deleteRecursive(filePath.c_str());
            else
                remove(filePath.c_str());
        }
        f_closedir(&dir);
        rmdir(path);
        return true;
    }
    else
    {
        return (remove(path) == 0);
    }
}

void SDWebServer::initRoutes()
{
    // Root page
    server->on("/file", HTTP_GET, [](AsyncWebServerRequest *request)
               { request->send(200, "text/html", index_html_app); });

    // Recursive list (returns JSON)
    server->on("/list", HTTP_GET, [this](AsyncWebServerRequest *request)
               {
        String json = this->listFilesRecursive(FAT_ROOT_WEB, SD_MOUNT_WEB, 12);
        request->send(200, "application/json", json); });

    // Download (chunked streaming)
    server->on("/download", HTTP_GET, [this](AsyncWebServerRequest *request)
               {
    if (!request->hasParam("file")) {
        request->send(400, "text/plain", "missing file param");
        return;
    }

    String relPath = request->getParam("file")->value(); // e.g. "/myfolder/file.bin" or "/sdcard/myfolder/file.bin"
    String vpath = relPath;

    // Ensure path starts with /sdcard
    if (!vpath.startsWith(SD_MOUNT_WEB)) {
        vpath = String(SD_MOUNT_WEB) + (relPath.startsWith("/") ? "" : "/") + relPath;
    }

    Serial.printf("Download file: %s -> %s\n", relPath.c_str(), vpath.c_str());

    FILE *f = fopen(vpath.c_str(), "rb");
    if (!f) {
        request->send(404, "text/plain", "file not found");
        return;
    }

    String filename = vpath.substring(vpath.lastIndexOf('/') + 1);

    AsyncWebServerResponse *response = request->beginChunkedResponse(
        "application/octet-stream",
        [f](uint8_t *buffer, size_t maxLen, size_t index) mutable -> size_t {
            size_t n = fread(buffer, 1, maxLen, f);
            if (n == 0) fclose(f);
            return n;
        });

    response->addHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
    request->send(response); });

    // Delete file
    server->on("/delete", HTTP_GET, [this](AsyncWebServerRequest *request)
               {
    if (!request->hasParam("file")) {
        request->send(400, "text/plain", "missing file param");
        return;
    }

    String relPath = request->getParam("file")->value();
    String vpath = relPath;

    // Ensure path starts with /sdcard
    if (!vpath.startsWith(SD_MOUNT_WEB)) {
        vpath = String(SD_MOUNT_WEB) + (relPath.startsWith("/") ? "" : "/") + relPath;
    }

    Serial.printf("Delete requested: %s -> %s\n", relPath.c_str(), vpath.c_str());

    // Prevent deleting the root mount
    if (vpath.equals(String(SD_MOUNT_WEB)) || vpath.equals(String(SD_MOUNT_WEB) + "/")) {
        request->send(403, "text/plain", "cannot delete root folder");
        return;
    }

    struct stat st;
    if (stat(vpath.c_str(), &st) != 0) {
        request->send(404, "text/plain", "not found");
        return;
    }

    int rc;
    if (S_ISDIR(st.st_mode)) {
        // recursive delete for folders
        rc = deleteRecursive(vpath.c_str()) ? 0 : -1;
    } else {
        rc = remove(vpath.c_str());
    }

    if (rc == 0)
        request->send(200, "text/plain", "deleted");
    else
        request->send(500, "text/plain", "delete failed"); });
    // Upload
    server->on(
    "/upload", HTTP_POST,
    [](AsyncWebServerRequest *request) {
        request->send(200, "text/plain", "upload ok");
    },
    [this](AsyncWebServerRequest *request, String filename, size_t index,
           uint8_t *data, size_t len, bool final)
    {
        FILE *&f = (FILE *&)request->_tempObject;

        if (index == 0) {
            // --- Get "dir" field from POST ---
            String dir = "/";
            const AsyncWebParameter *p = request->getParam("dir", true, false);
            if (p) {
                dir = p->value();  // e.g. "/sdcard/pic"
            }

            // --- Normalize ---
            if (dir == "/" || dir.length() == 0) {
                dir = "";
            } else if (!dir.startsWith("/")) {
                dir = "/" + dir;
            }

            // --- Final absolute path ---
            String vpath = String(SD_MOUNT_WEB) + dir + "/" + filename;

            Serial.printf("Upload start: %s -> %s\n",
                          filename.c_str(), vpath.c_str());

            f = fopen(vpath.c_str(), "wb");
            if (!f) {
                Serial.printf("!! fopen failed at path: %s\n", vpath.c_str());
                return;
            }
        }

        if (f && len) {
            fwrite(data, 1, len, f);
        }

        if (final) {
            if (f) {
                fclose(f);
                f = nullptr;
            }
            Serial.printf("Upload complete: %s (%u bytes)\n",
                          filename.c_str(), (unsigned)(index + len));
        }
    });



    // Make directory
    server->on("/mkdir", HTTP_POST, [this](AsyncWebServerRequest *request)
               {
        if (!request->hasParam("parent", true) || !request->hasParam("name", true)) {
            request->send(400, "text/plain", "missing parent or name");
            return;
        }
        String parent = request->getParam("parent", true)->value();
        String name   = request->getParam("name", true)->value();

        if (!parent.startsWith("/"))
            parent = "/" + parent;

        String vpath = joinPath(SD_MOUNT_WEB, joinPath(parent, name));
        int rc = mkdir(vpath.c_str(), 0777);
        Serial.println(vpath);

        if (rc == 0)
            request->send(200, "text/plain", "mkdir ok");
        else
            request->send(500, "text/plain", "mkdir failed"); });
    server->on("/delete-multi", HTTP_POST, [this](AsyncWebServerRequest *request)
               {
    if (request->contentType() != "application/json") {
        request->send(400, "text/plain", "expected json");
        return;
    } }, NULL, [this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
               {
    static String body;
    if (index == 0) body = "";
    body += String((const char*)data).substring(0, len);
    if (index + len == total) {
        // parse JSON manually (simple format)
        DynamicJsonDocument doc(2048);
        if (deserializeJson(doc, body) != DeserializationError::Ok) {
            request->send(400, "text/plain", "bad json");
            return;
        }

        JsonArray arr = doc["files"].as<JsonArray>();
        int success = 0;
        for (JsonVariant v : arr) {
            String vpath = v.as<String>();
            if (vpath.startsWith(SD_MOUNT_WEB)) {
                if (deleteRecursive(vpath.c_str())) success++;
            }
        }

        String msg = "Deleted " + String(success) + " / " + String(arr.size()) + " items";
        request->send(200, "text/plain", msg);
    } });
}
