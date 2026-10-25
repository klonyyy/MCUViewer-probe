/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SetupPortal.hpp"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "Wifi.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

namespace
{
constexpr const char* tag = "setup";

/* The whole page, with no external resources: the access point has no internet */
constexpr char page[] = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>NETWORK PROBE setup</title>
<style>
:root{color-scheme:light dark;--fg:#1c1c1e;--bg:#f2f2f5;--card:#fff;--line:#d1d1d6;--muted:#6c6c70;--accent:#0a66c2;--ok:#1a7f37;--bad:#c62828}
@media (prefers-color-scheme:dark){:root{--fg:#f2f2f5;--bg:#111113;--card:#1c1c1e;--line:#3a3a3c;--muted:#a0a0a6;--accent:#4c9aff;--ok:#3fb950;--bad:#ff6b6b}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
main{max-width:30rem;margin:0 auto;padding:16px}
h1{font-size:1.3rem;margin:8px 0 0}
#id{margin:0 0 16px;color:var(--muted);font-size:.9rem}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:16px;margin-bottom:16px;overflow-wrap:anywhere}
.card p{margin:0 0 8px}
.card p:last-child{margin:0}
#status{display:none;border-left-width:4px}
#status.ok{border-color:var(--ok)}
#status.bad{border-color:var(--bad)}
.head{display:flex;justify-content:space-between;align-items:baseline;margin-bottom:8px}
h2{font-size:1rem;margin:0}
.link{border:0;background:none;padding:0;color:var(--accent);font:inherit;font-size:.9rem;cursor:pointer}
.link:disabled{color:var(--muted);cursor:default}
#nets{color:var(--muted);font-size:.9rem}
.net{display:flex;justify-content:space-between;align-items:center;gap:8px;width:100%;margin:0 0 6px;padding:10px 12px;border:1px solid var(--line);border-radius:8px;background:none;color:var(--fg);font:inherit;text-align:left;cursor:pointer}
.net span:first-child{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.net.sel{border-color:var(--accent);box-shadow:0 0 0 1px var(--accent)}
.sig{display:flex;align-items:center;gap:6px;color:var(--muted);font-size:.8rem}
.bars{display:inline-flex;align-items:flex-end;gap:2px;height:14px}
.bars i{width:4px;background:var(--line);border-radius:1px}
.bars i.on{background:var(--fg)}
label{display:block;margin:14px 0 4px;color:var(--muted);font-size:.9rem}
input[type=text],input[type=password]{width:100%;padding:10px;border:1px solid var(--line);border-radius:8px;background:var(--bg);color:var(--fg);font:inherit}
.check{display:flex;align-items:center;gap:6px;margin-top:6px}
.check label{margin:0}
.hint{margin:6px 0 0;color:var(--muted);font-size:.85rem}
code{font-family:ui-monospace,Menlo,Consolas,monospace;font-size:.9em}
#join{width:100%;margin-top:18px;padding:12px;border:0;border-radius:8px;background:var(--accent);color:#fff;font:inherit;font-weight:600;cursor:pointer}
#join:disabled{opacity:.5}
</style>
</head>
<body>
<main>
<h1>NETWORK PROBE</h1>
<p id="id">Setup</p>
<div class="card" id="status"></div>
<div class="card">
<div class="head"><h2>Wi-Fi network</h2><button class="link" id="rescan" type="button">Scan again</button></div>
<div id="nets">Scanning...</div>
<label for="ssid">Network name</label>
<input type="text" id="ssid" maxlength="32" autocomplete="off" autocapitalize="off" spellcheck="false">
<label for="pass">Password</label>
<input type="password" id="pass" maxlength="64" autocomplete="off">
<div class="check"><input type="checkbox" id="show"><label for="show">Show password</label></div>
<label for="host">Hostname</label>
<input type="text" id="host" maxlength="32" autocomplete="off" autocapitalize="off" spellcheck="false">
<p class="hint">MCUViewer and GDB reach the probe at <code id="fqdn"></code></p>
<button id="join" type="button">Connect</button>
</div>
</main>
<script>
"use strict";
const $ = (id) => document.getElementById(id);
const sleep = (ms) => new Promise((done) => setTimeout(done, ms));
let last = null;
let deviceId = "";
let joined = false;
let missed = 0;

async function get(url) {
  const response = await fetch(url, {cache: "no-store"});
  if (!response.ok) throw new Error(response.status);
  return response.json();
}

function bars(rssi) {
  const level = rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -78 ? 2 : 1;
  const box = document.createElement("span");
  box.className = "bars";
  for (let i = 1; i <= 4; i++) {
    const bar = document.createElement("i");
    bar.style.height = i * 25 + "%";
    if (i <= level) bar.className = "on";
    box.append(bar);
  }
  return box;
}

function showNetworks(networks) {
  const box = $("nets");
  box.textContent = networks.length ? "" : "No networks found. Type the name below.";
  for (const network of networks) {
    const button = document.createElement("button");
    button.type = "button";
    button.className = "net";
    const name = document.createElement("span");
    name.textContent = network.ssid;
    const signal = document.createElement("span");
    signal.className = "sig";
    if (!network.secure) signal.append("open");
    signal.append(bars(network.rssi));
    button.append(name, signal);
    button.onclick = () => {
      for (const other of box.children) other.classList.remove("sel");
      button.classList.add("sel");
      $("ssid").value = network.ssid;
      $("pass").value = "";
      (network.secure ? $("pass") : $("join")).focus();
    };
    box.append(button);
  }
}

async function scan(refresh) {
  $("rescan").disabled = true;
  $("nets").textContent = "Scanning...";
  let url = refresh ? "/scan?refresh=1" : "/scan";
  for (let tries = 0; tries < 30; tries++) {
    try {
      const result = await get(url);
      url = "/scan";
      if (!result.scanning) {
        showNetworks(result.networks);
        $("rescan").disabled = false;
        return;
      }
    } catch (error) {}
    await sleep(1000);
  }
  $("nets").textContent = "The scan did not finish. Type the name below.";
  $("rescan").disabled = false;
}

function say(kind, ...lines) {
  const box = $("status");
  box.className = "card " + kind;
  box.style.display = lines.length ? "block" : "none";
  box.textContent = "";
  for (const line of lines) {
    const paragraph = document.createElement("p");
    for (const part of [].concat(line)) {
      if (typeof part === "string") {
        paragraph.append(part);
      } else {
        const code = document.createElement("code");
        code.textContent = part.code;
        paragraph.append(code);
      }
    }
    box.append(paragraph);
  }
}

function showStatus(status) {
  const name = {code: status.hostname + ".local"};
  if (status.state === "connecting") {
    say("", "Connecting to " + status.ssid + "...");
  } else if (status.state === "connected") {
    say("ok",
        ["Connected to " + status.ssid + " as ", name, " (" + status.ip + ")."],
        ["In MCUViewer choose the NETWORK_PROBE probe with the address ", name, " and the port ", {code: String(status.port)}, "."],
        "This setup network closes in about a minute. Connect this device back to your own network.");
  } else if (status.state === "failed") {
    say("bad", "Could not connect to " + status.ssid + ": " + status.error + ".");
  } else if (status.saved) {
    say("", "The saved network " + status.saved + " is out of reach. The probe tries it again every 30 seconds.");
  } else {
    say("");
  }
}

function showHost() {
  $("fqdn").textContent = ($("host").value.trim().toLowerCase() || deviceId) + ".local";
}

async function poll() {
  for (;;) {
    try {
      const status = await get("/status");
      missed = 0;
      if (!last) {
        deviceId = status.id;
        $("id").textContent = status.id;
        $("host").value = status.hostname;
        $("host").placeholder = status.id;
        showHost();
      }
      last = status;
      showStatus(status);
    } catch (error) {
      if (++missed === 3 && last) {
        if (joined || last.state === "connected") {
          say("", ["The probe stopped answering: it has probably joined " + last.ssid + " and closed this setup network. Connect this device back to your network and use ", {code: last.hostname + ".local"}, "."]);
        } else {
          say("bad", "The probe does not answer. Is this device still connected to " + last.id + "?");
        }
      }
    }
    await sleep(1000);
  }
}

$("join").onclick = async () => {
  const ssid = $("ssid").value;
  const form = new URLSearchParams({ssid: ssid, password: $("pass").value, hostname: $("host").value.trim().toLowerCase()});
  $("join").disabled = true;
  try {
    const response = await fetch("/join", {method: "POST", body: form});
    if (response.ok) {
      joined = true;
      say("", "Connecting to " + ssid + "...");
    } else {
      say("bad", await response.text());
    }
  } catch (error) {
    say("bad", "The probe did not answer. Try again.");
  }
  $("join").disabled = false;
};
$("show").onchange = () => { $("pass").type = $("show").checked ? "text" : "password"; };
$("pass").onkeydown = (event) => { if (event.key === "Enter") $("join").click(); };
$("host").oninput = showHost;
$("rescan").onclick = () => scan(true);
scan(false);
poll();
</script>
</body>
</html>
)html";

const char* toString(const Wifi::JoinState state)
{
	switch (state)
	{
		case Wifi::JoinState::connecting:
			return "connecting";
		case Wifi::JoinState::connected:
			return "connected";
		case Wifi::JoinState::failed:
			return "failed";
		default:
			return "idle";
	}
}

/* Appends text to the buffer that ends at end, which stays terminated; what does not fit is cut */
char* putText(char* out, char* const end, const char* text)
{
	while (*text && out < end - 1)
		*out++ = *text++;
	*out = '\0';
	return out;
}

/* Appends text as a quoted JSON string; SSIDs may hold any byte */
char* putJsonString(char* out, char* const end, const char* text)
{
	out = putText(out, end, "\"");
	for (; *text; ++text)
	{
		const unsigned char c = static_cast<unsigned char>(*text);
		char escaped[8] = {static_cast<char>(c), '\0'};
		if (c == '"' || c == '\\')
		{
			escaped[0] = '\\';
			escaped[1] = static_cast<char>(c);
			escaped[2] = '\0';
		}
		else if (c < 0x20)
			snprintf(escaped, sizeof(escaped), "\\u%04x", c);
		out = putText(out, end, escaped);
	}
	return putText(out, end, "\"");
}

int hexValue(const char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Decodes application/x-www-form-urlencoded text in place: '+' is a space, %XX a byte. False if malformed. */
bool urlDecode(char* const text)
{
	char* out = text;
	for (const char* in = text; *in; ++in)
	{
		if (*in == '+')
			*out++ = ' ';
		else if (*in == '%')
		{
			const int high = hexValue(in[1]);
			const int low = high < 0 ? -1 : hexValue(in[2]);
			if (low < 0 || (high == 0 && low == 0))
				return false;
			*out++ = static_cast<char>(high * 16 + low);
			in += 2;
		}
		else
			*out++ = *in;
	}
	*out = '\0';
	return true;
}

/* Reads the form of the setup page into settings. Returns an error for the page, or nullptr. */
const char* readJoinForm(char* const form, NetworkSettings& settings)
{
	struct Field
	{
		const char* key;
		char* value;
		size_t size;
		const char* tooLong;
	};
	const Field fields[] = {
		{"ssid", settings.ssid, sizeof(settings.ssid), "The network name has more than 32 bytes."},
		{"password", settings.password, sizeof(settings.password), "The password has more than 64 characters."},
		{"hostname", settings.hostname, sizeof(settings.hostname), "The hostname has more than 32 characters."},
	};

	char* save = nullptr;
	for (char* pair = strtok_r(form, "&", &save); pair; pair = strtok_r(nullptr, "&", &save))
	{
		char* const separator = strchr(pair, '=');
		if (!separator)
			continue;
		*separator = '\0';
		char* const value = separator + 1;
		if (!urlDecode(pair) || !urlDecode(value))
			return "The form could not be read.";
		for (const Field& field : fields)
		{
			if (strcmp(pair, field.key))
				continue;
			if (strlen(value) >= field.size)
				return field.tooLong;
			strlcpy(field.value, value, field.size);
		}
	}
	return nullptr;
}

esp_err_t sendJson(httpd_req_t* const request, const char* const json)
{
	httpd_resp_set_type(request, "application/json");
	httpd_resp_set_hdr(request, "Cache-Control", "no-store");
	return httpd_resp_sendstr(request, json);
}

esp_err_t sendError(httpd_req_t* const request, const char* const message)
{
	httpd_resp_set_status(request, "400 Bad Request");
	httpd_resp_set_type(request, "text/plain");
	return httpd_resp_sendstr(request, message);
}
}  // namespace

void SetupPortal::start(esp_netif_t* const apNetif)
{
	esp_netif_ip_info_t ipInfo = {};
	esp_netif_get_ip_info(apNetif, &ipInfo);
	apAddress = ipInfo.ip.addr;
	snprintf(pageUrl, sizeof(pageUrl), "http://" IPSTR "/", IP2STR(&ipInfo.ip));

	/* DHCP option 114 (RFC 8910): clients that know it open the page without probing for a portal */
	ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_option(apNetif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, pageUrl, strlen(pageUrl)));

	/* The portal probes of phones and laptops end in redirects and dropped connections, which these would log */
	esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
	esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
	esp_log_level_set("httpd_parse", ESP_LOG_ERROR);

	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	config.stack_size = httpTaskStackSize;
	config.core_id = 0;
	config.max_open_sockets = httpMaxOpenSockets;
	config.lru_purge_enable = true;
	config.global_user_ctx = this;
	config.global_user_ctx_free_fn = [](void*) {}; /* Without one, httpd_stop() would free() this */
	if (httpd_start(&server, &config) == ESP_OK)
	{
		const auto add = [this](const char* const uri, const httpd_method_t method, esp_err_t (*const handler)(httpd_req_t*))
		{
			httpd_uri_t entry = {};
			entry.uri = uri;
			entry.method = method;
			entry.handler = handler;
			httpd_register_uri_handler(server, &entry);
		};
		add("/", HTTP_GET, handlePage);
		add("/status", HTTP_GET, handleStatus);
		add("/scan", HTTP_GET, handleScan);
		add("/join", HTTP_POST, handleJoin);
		httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, redirect);
	}
	else
	{
		ESP_LOGE(tag, "the HTTP server did not start");
		server = nullptr;
	}

	dnsRunning.store(true, std::memory_order_relaxed);
	xTaskCreatePinnedToCore(dnsTaskEntry, "dns", dnsTaskStackSize, this, dnsTaskPriority, nullptr, 0);
	ESP_LOGI(tag, "setup page at %s", pageUrl);
}

void SetupPortal::stop()
{
	if (server)
	{
		httpd_stop(server);
		server = nullptr;
	}
	/* The DNS task sees it within its receive timeout and ends */
	dnsRunning.store(false, std::memory_order_relaxed);
}

SetupPortal& SetupPortal::of(httpd_req_t* const request)
{
	return *static_cast<SetupPortal*>(httpd_get_global_user_ctx(request->handle));
}

esp_err_t SetupPortal::handlePage(httpd_req_t* const request)
{
	httpd_resp_set_type(request, "text/html; charset=utf-8");
	httpd_resp_set_hdr(request, "Cache-Control", "no-store");
	return httpd_resp_send(request, page, sizeof(page) - 1);
}

esp_err_t SetupPortal::handleStatus(httpd_req_t* const request)
{
	Wifi& wifi = of(request).wifi;
	const Wifi::Status status = wifi.getStatus();

	char json[1024];
	char* const end = json + sizeof(json);
	char* out = putText(json, end, "{\"id\":");
	out = putJsonString(out, end, wifi.getDeviceId());
	out = putText(out, end, ",\"state\":");
	out = putJsonString(out, end, toString(status.state));
	out = putText(out, end, ",\"ssid\":");
	out = putJsonString(out, end, status.ssid);
	out = putText(out, end, ",\"hostname\":");
	out = putJsonString(out, end, status.hostname);
	out = putText(out, end, ",\"saved\":");
	out = putJsonString(out, end, status.savedSsid);
	out = putText(out, end, ",\"ip\":");
	out = putJsonString(out, end, status.ip);
	out = putText(out, end, ",\"error\":");
	out = putJsonString(out, end, status.error);
	snprintf(out, end - out, ",\"port\":%d}", CONFIG_NETWORK_PROBE_PORT);
	return sendJson(request, json);
}

esp_err_t SetupPortal::handleScan(httpd_req_t* const request)
{
	Wifi& wifi = of(request).wifi;
	char query[32] = {};
	const bool refresh = httpd_req_get_url_query_str(request, query, sizeof(query)) == ESP_OK && strstr(query, "refresh=1");
	wifi.requestScan(refresh);

	Wifi::Network networks[Wifi::maxNetworks];
	bool scanning = false;
	const size_t count = wifi.getNetworks(networks, Wifi::maxNetworks, scanning);

	httpd_resp_set_type(request, "application/json");
	httpd_resp_set_hdr(request, "Cache-Control", "no-store");
	char chunk[256];
	snprintf(chunk, sizeof(chunk), "{\"scanning\":%s,\"networks\":[", scanning ? "true" : "false");
	httpd_resp_sendstr_chunk(request, chunk);
	for (size_t i = 0; i < count; ++i)
	{
		char* const end = chunk + sizeof(chunk);
		char* out = putText(chunk, end, i ? ",{\"ssid\":" : "{\"ssid\":");
		out = putJsonString(out, end, networks[i].ssid);
		snprintf(out, end - out, ",\"rssi\":%d,\"secure\":%s}", networks[i].rssi, networks[i].secured ? "true" : "false");
		httpd_resp_sendstr_chunk(request, chunk);
	}
	httpd_resp_sendstr_chunk(request, "]}");
	return httpd_resp_sendstr_chunk(request, nullptr);
}

esp_err_t SetupPortal::handleJoin(httpd_req_t* const request)
{
	Wifi& wifi = of(request).wifi;
	char form[maxFormSize];
	if (request->content_len >= sizeof(form))
		return sendError(request, "The form is too long.");
	size_t received = 0;
	while (received < request->content_len)
	{
		const int length = httpd_req_recv(request, form + received, request->content_len - received);
		if (length <= 0)
			return ESP_FAIL;
		received += static_cast<size_t>(length);
	}
	form[received] = '\0';

	NetworkSettings settings;
	const char* error = readJoinForm(form, settings);
	if (!error)
	{
		for (char* c = settings.hostname; *c; ++c)
			if (*c >= 'A' && *c <= 'Z')
				*c = static_cast<char>(*c - 'A' + 'a');
		if (!settings.hostname[0])
			strlcpy(settings.hostname, wifi.getDeviceId(), sizeof(settings.hostname));
		error = settings.validate();
	}
	if (error)
		return sendError(request, error);

	wifi.join(settings);
	httpd_resp_set_type(request, "text/plain");
	return httpd_resp_sendstr(request, "OK");
}

esp_err_t SetupPortal::redirect(httpd_req_t* const request, httpd_err_code_t)
{
	httpd_resp_set_status(request, "302 Found");
	httpd_resp_set_hdr(request, "Location", of(request).pageUrl);
	/* iOS sees the portal only with a body */
	return httpd_resp_sendstr(request, "NETWORK PROBE setup");
}

void SetupPortal::dnsTaskEntry(void* const arg)
{
	static_cast<SetupPortal*>(arg)->runDns();
	vTaskDelete(nullptr);
}

void SetupPortal::runDns()
{
	const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_port = htons(dnsPort);
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	if (sock < 0 || bind(sock, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
	{
		ESP_LOGE(tag, "the DNS server cannot listen on port %u: errno %d", dnsPort, errno);
		if (sock >= 0)
			close(sock);
		return;
	}
	/* Wakes up now and then to see whether to stop */
	timeval timeout = {};
	timeout.tv_sec = 1;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

	uint8_t packet[dnsMaxQuery + dnsAnswerSize];
	while (dnsRunning.load(std::memory_order_relaxed))
	{
		sockaddr_in peer;
		socklen_t peerLength = sizeof(peer);
		const ssize_t received = recvfrom(sock, packet, dnsMaxQuery, 0, reinterpret_cast<sockaddr*>(&peer), &peerLength);
		if (received <= 0)
			continue;
		const size_t length = answerDns(packet, static_cast<size_t>(received));
		if (length)
			sendto(sock, packet, length, 0, reinterpret_cast<const sockaddr*>(&peer), peerLength);
	}
	close(sock);
}

size_t SetupPortal::answerDns(uint8_t* const packet, const size_t length) const
{
	/* The header: id, flags, then the counts of questions, answers, authority and additional records, big endian */
	if (length < dnsHeaderSize || (packet[2] & 0x80))
		return 0; /* Too short, or not a query */

	const uint8_t opcode = (packet[2] >> 3) & 0x0F;
	const uint16_t questions = static_cast<uint16_t>((packet[4] << 8) | packet[5]);
	uint8_t rcode = 0;
	size_t end = dnsHeaderSize;
	bool answer = false;
	if (opcode != 0)
		rcode = 4; /* Not implemented */
	else if (questions != 1)
		rcode = 1; /* Format error */
	else
	{
		/* The name: labels up to the root label, never compressed in a question */
		while (end < length && packet[end] != 0)
		{
			if (packet[end] > 63)
				return 0;
			end += 1U + packet[end];
		}
		if (end + 5 > length)
			return 0;
		const uint16_t type = static_cast<uint16_t>((packet[end + 1] << 8) | packet[end + 2]);
		const uint16_t qclass = static_cast<uint16_t>((packet[end + 3] << 8) | packet[end + 4]);
		end += 5;
		/* A or ANY, class IN; anything else gets an empty answer, not an error */
		answer = (type == 1 || type == 255) && qclass == 1;
	}

	/* Response, authoritative, recursion desired as asked, no recursion available */
	packet[2] = static_cast<uint8_t>(0x80 | (opcode << 3) | 0x04 | (packet[2] & 0x01));
	packet[3] = rcode;
	packet[4] = 0;
	packet[5] = rcode ? 0 : 1;
	packet[6] = 0;
	packet[7] = answer ? 1 : 0;
	/* No authority or additional records: the EDNS record of the query is left out */
	memset(packet + 8, 0, 4);
	if (rcode)
		return dnsHeaderSize;

	if (answer)
	{
		uint8_t* const record = packet + end;
		record[0] = 0xC0; /* The name: a pointer to the question's */
		record[1] = dnsHeaderSize;
		record[2] = 0; /* A */
		record[3] = 1;
		record[4] = 0; /* IN */
		record[5] = 1;
		record[6] = static_cast<uint8_t>(dnsTtl >> 24);
		record[7] = static_cast<uint8_t>(dnsTtl >> 16);
		record[8] = static_cast<uint8_t>(dnsTtl >> 8);
		record[9] = static_cast<uint8_t>(dnsTtl);
		record[10] = 0;
		record[11] = 4;
		memcpy(record + 12, &apAddress, 4);
		end += dnsAnswerSize;
	}
	return end;
}
