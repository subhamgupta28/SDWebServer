#pragma once
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include <dirent.h>
#include <sys/stat.h> // mkdir
#include <stdio.h>
#include <string.h>
#define SD_MOUNT_WEB "/sdcard"
#define FAT_ROOT_WEB "0:/"


    const char index_html_app[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>SD File Server</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 20px; }
    h2 { color: #333; }
    ul { list-style: none; padding-left: 18px; }
    .folder { font-weight: bold; cursor: pointer; user-select:none; }
    .file { margin-left: 4px; }
    .hidden { display: none; }
    .row { display:flex; gap:8px; align-items:center; margin: 8px 0; }
    button { cursor:pointer; }
    .mono { font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, "Liberation Mono", monospace; }
  </style>
</head>
<body>
  <h2>SD File Manager</h2>

  <div class="row">
    <form id="uploadForm" method="POST" action="/upload" enctype="multipart/form-data">
      <input type="file" name="upload" multiple required>
      <select id="folderSelect" name="dir" class="mono"></select>
      <input type="submit" value="Upload">
    </form>

    <form id="mkdirForm" method="POST" action="/mkdir">
      <select id="mkdirParent" name="parent" class="mono"></select>
      <input type="text" name="name" placeholder="New folder name" required>
      <input type="submit" value="Create Folder">
    </form>
  </div>

  <h3>Files & Folders</h3>
  <div id="filelist"></div>

  <script>
    async function fetchJSON(url) {
      const r = await fetch(url);
      if (!r.ok) throw new Error("HTTP " + r.status);
      return r.json();
    }

    async function loadAll() {
      const data = await fetchJSON('/list');
      const container = document.getElementById('filelist');
      container.innerHTML = "";
      container.appendChild(renderTree(data));

      // fill folder selects
      const folders = [];
      collectFolders(data, folders); // array of {name, path, depth}
      const sel1 = document.getElementById('folderSelect');
      const sel2 = document.getElementById('mkdirParent');
      sel1.innerHTML = "";
      sel2.innerHTML = "";
      folders.forEach(f => {
        const label = " ".repeat(f.depth*2) + (f.path === "/sdcard" ? "/" : f.name + "/");
        const opt1 = document.createElement('option');
        opt1.value = f.path;
        opt1.textContent = label;
        sel1.appendChild(opt1);
        const opt2 = document.createElement('option');
        opt2.value = f.path;
        opt2.textContent = label;
        sel2.appendChild(opt2);
      });
    }

    function collectFolders(items, out, depth=0) {
      // ensure root exists in list (when called first time)
      if (depth === 0) {
        out.push({name:"/", path:"/sdcard", depth:0});
      }
      items.forEach(it => {
        if (it.type === "dir") {
          out.push({name: it.name, path: it.path, depth});
          if (it.children && it.children.length) {
            collectFolders(it.children, out, depth+1);
          }
        }
      });
    }

    function renderTree(items) {
      const ul = document.createElement('ul');
      items.forEach(item => {
        const li = document.createElement('li');
        if (item.type === "dir") {
          const span = document.createElement('span');
          span.className = "folder";
          span.textContent = "📂 " + item.name;
          span.onclick = () => {
            const child = li.querySelector('ul');
            if (child) child.classList.toggle('hidden');
          };
          li.appendChild(span);

          // 🆕 Delete button for folders
          const btn = document.createElement('button');
          btn.textContent = "Delete";
          btn.onclick = async (e) => {
            e.preventDefault();
            if (confirm("Delete folder " + item.path + " and all its contents?")) {
              await fetch('/delete?file=' + encodeURIComponent(item.path));
              loadAll();
            }
          };
          li.appendChild(document.createTextNode(" "));
          li.appendChild(btn);

          if (item.children && item.children.length) {
            const child = renderTree(item.children);
            child.classList.add('hidden'); // collapsed by default
            li.appendChild(child);
          }
        } else {
          li.className = "file";
          const a = document.createElement('a');
          a.href = "/download?file=" + encodeURIComponent(item.path);
          a.textContent = "📄 " + item.name + " (" + item.size + " bytes)";
          const btn = document.createElement('button');
          btn.textContent = "Delete";
          btn.onclick = async (e) => {
            e.preventDefault();
            if (confirm("Delete " + item.path + " ?")) {
              await fetch('/delete?file=' + encodeURIComponent(item.path));
              loadAll();
            }
          };
          li.appendChild(a);
          li.appendChild(document.createTextNode(" "));
          li.appendChild(btn);
        }
        ul.appendChild(li);
      });
      return ul;
    }

    // refresh on successful forms
    document.getElementById('uploadForm').addEventListener('submit', async (e) => {
      // let normal form post happen; page will not reload because server returns text
      // after a short wait, refresh listing
      setTimeout(loadAll, 500);
    });

    document.getElementById('mkdirForm').addEventListener('submit', async (e) => {
      e.preventDefault();
      const form = e.target;
      const body = new FormData(form);
      const r = await fetch('/mkdir', { method:'POST', body });
      if (!r.ok) alert("mkdir failed");
      await loadAll();
      form.reset();
    });

    loadAll();
  </script>
</body>
</html>
)HTML";



class SDWebServer
{
public:
    // Constructor: create own server
    SDWebServer(int port = 80);

    // Constructor: reuse existing server
    SDWebServer(AsyncWebServer &existingServer);

    ~SDWebServer();

    bool begin();

private:
    AsyncWebServer *server; // always use pointer internally
    bool ownsServer;
    sdmmc_card_t *card = NULL;
    void initSD();
    void initRoutes();
    String listFilesRecursive(const String &fatDir, const String &vfsDir, uint8_t levels);
    bool deleteRecursive(const char *path);
    bool deleteRecursiveOld(const char *path);
};
