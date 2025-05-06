/*
This example uses Win32 API for a system tray (notification area) icon in a C++ Visual Studio project. It assumes you have `.ico` files for the buttons and a connection logic implemented elsewhere.
Now with persistent preference storage and background validation of device connection.
*/

#define WIN32_LEAN_AND_MEAN
//#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
//#include <wininet.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <objbase.h>
#include <strsafe.h>
#include <Shlwapi.h>

//#include <algorithm>
#include <string>
#include <iostream>
#include <vector>
#include <sstream>
#include <fstream>
#include <thread>
#include <mutex>
#include <functional>

#include <json/json.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "Shlwapi.lib")

#include "Resource.h"

#define TRAY_ICON_UID 1
#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_EXIT 1001
#define ID_TRAY_CONNECT 1002
#define ID_TRAY_DEVICE_INFO 1003
#define ID_TRAY_STARTUP 1004
#define ID_BUTTON_PLAY_PAUSE 2001
#define ID_BUTTON_PAUSE 2002
#define ID_BUTTON_MUTE 2003
#define ID_BUTTON_VOL_DOWN 2004
#define ID_BUTTON_VOL_UP 2005
#define ID_BUTTON_OPTICAL 2006
#define TRAY_ICON_TOOLTIP L"HEOS Controller"

HINSTANCE hInst;
HWND hwndMain;
HWND hwndToolbar = NULL;
NOTIFYICONDATA nid;
bool isConnected = false;
bool isMuted = false;
//bool isPlaying = false;
//std::string currentInput = "";
std::string deviceName = "Not connected";
std::string deviceIP = "";
std::string devicePID = "";
const char* PREFS_FILE = "prefs.json";

// Forward declarations
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void ShowContextMenu(HWND, POINT);
void ShowButtonToolbar();
void ValidateConnection();
void SlackMonitorLoop();

// Custom stream buffer that redirects output to OutputDebugString
class OutputDebugStreamBuf : public std::streambuf {
protected:
	// Override the overflow function to capture characters and send to OutputDebugStringW
	int overflow(int c) override {
		if (c != EOF) {
			char ch = static_cast<char>(c);

			// Convert char to wchar_t using MultiByteToWideChar
			wchar_t wch[2] = { 0 };  // Buffer for single wchar_t character
			MultiByteToWideChar(CP_ACP, 0, &ch, 1, wch, 1);

			OutputDebugStringW(wch);  // Send the wide character to OutputDebugStringW
		}
		return c;
	}

	// Override the sync function to handle flushing
	int sync() override {
		return 0;
	}
};

// Redirect std::cout to OutputDebugString
class OutputDebugStream : public std::ostream {
public:
	OutputDebugStream() : std::ostream(&buf) {}

private:
	OutputDebugStreamBuf buf;
};

OutputDebugStream out;  // Create a custom output stream

struct HeosPlayer {
	std::string name;
	std::string ip;
	std::string pid;
};

std::wstring ToWString(const std::string& str) {
	return std::wstring(str.begin(), str.end());
}

//void ShowBalloonTip(const std::string& title, const std::string& message, int iconID) {
//	const int BALLOON_TIMEOUT = 3000; // in milliseconds
//
//	HICON hIcon = (HICON)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(iconID), IMAGE_ICON, 0, 0, LR_SHARED);
//
//	ZeroMemory(&nid, sizeof(nid));
//	nid.cbSize = sizeof(NOTIFYICONDATA);
//	nid.hWnd = hwndMain;
//	nid.uID = TRAY_ICON_UID;
//	nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
//	nid.uCallbackMessage = WM_TRAYICON;
//	nid.hIcon = hIcon;
//	nid.uFlags = NIF_INFO;
//	wcscpy_s(nid.szTip, TRAY_ICON_TOOLTIP);
//	wcscpy_s(nid.szInfoTitle, ToWString(title).c_str());
//	wcscpy_s(nid.szInfo, ToWString(message).c_str());
//	nid.dwInfoFlags = NIIF_INFO; // can also use NIIF_WARNING or NIIF_ERROR
//	nid.uTimeout = BALLOON_TIMEOUT;
//
//	Shell_NotifyIcon(NIM_MODIFY, &nid);
//}


std::string ToLowercase(const std::string& in) {
	std::string out = in;
	std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
	return out;
}

void AddAppToStartup() {
	WCHAR path[MAX_PATH];
	SHGetFolderPathW(NULL, CSIDL_STARTUP, NULL, 0, path);

	wcscat_s(path, L"\\HEOSController.lnk");

	// Don't create if it already exists
	if (PathFileExistsW(path)) return;

	WCHAR exePath[MAX_PATH];
	GetModuleFileNameW(NULL, exePath, MAX_PATH);

	CoInitialize(NULL);

	IShellLinkW* pShellLink = nullptr;
	if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pShellLink)))) {
		pShellLink->SetPath(exePath);
		pShellLink->SetDescription(L"HEOS Controller");
		pShellLink->SetWorkingDirectory(L".");

		IPersistFile* pPersistFile;
		if (SUCCEEDED(pShellLink->QueryInterface(IID_PPV_ARGS(&pPersistFile)))) {
			pPersistFile->Save(path, TRUE);
			pPersistFile->Release();
		}
		pShellLink->Release();
	}

	CoUninitialize();
}

void RemoveAppFromStartup() {
	WCHAR path[MAX_PATH];
	SHGetFolderPathW(NULL, CSIDL_STARTUP, NULL, 0, path);
	wcscat_s(path, L"\\HEOSController.lnk");

	DeleteFileW(path);
}

bool IsStartupEnabled() {
	WCHAR path[MAX_PATH];
	SHGetFolderPathW(NULL, CSIDL_STARTUP, NULL, 0, path);
	wcscat_s(path, L"\\HEOSController.lnk");
	return PathFileExistsW(path);
}

void SendHeosCommand(const std::string& command, const std::string& params = "", const std::function<void(const std::string&)>& callback = NULL)
{
	std::thread([command, params, callback] {
		if (deviceIP.length() < 4 + 3)
		{
			std::cout << "Device not ready; IP is empty: " << deviceIP << std::endl;
			return;
		}

		WSADATA wsaData;
		SOCKET sock = INVALID_SOCKET;
		sockaddr_in server;

		WSAStartup(MAKEWORD(2, 2), &wsaData);

		sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		server.sin_family = AF_INET;
		server.sin_port = htons(1255);
		inet_pton(AF_INET, deviceIP.c_str(), &server.sin_addr);

		std::string out;
		if (connect(sock, (SOCKADDR*)&server, sizeof(server)) == 0) {

			auto fullCommand = "heos://player/" + command;
			std::string app = "?";
			if (!devicePID.empty()) {
				fullCommand += app + "pid=" + devicePID;
				app = "&";
			}
			if (!params.empty()) {
				fullCommand += app + params;
				app = "&";
			}
			fullCommand += "\r\n";

			send(sock, fullCommand.c_str(), (int)fullCommand.length(), 0);

			// Optional: receive response
			char buffer[1024];
			int bytesReceived = recv(sock, buffer, sizeof(buffer) - 1, 0);
			if (bytesReceived > 0) {
				buffer[bytesReceived] = '\0';
				out = buffer;
				std::cout << buffer << std::endl;
			}
		}
		else {
			std::cerr << "Failed to connect to HEOS device." << std::endl;
		}

		closesocket(sock);
		WSACleanup();

		if (callback != NULL)
		{
			callback(out);
		}
		}).detach();
}

//bool SendHttpRequest(const std::string & command, const std::string& params = "") {
//	std::string url = "http://" + deviceIP + "/heos/v1/player/" + command + "?pid=" + devicePID;
//	if (!params.empty()) {
//		url += "&" + params;
//	}
//
//	HINTERNET hInternet, hConnect;
//	DWORD bytesRead;
//	bool success = false;
//
//	// Initialize WinINet
//	hInternet = InternetOpen(L"HEOS Control", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
//	if (hInternet != NULL) {
//		// Open a connection to the HEOS device (replace with your device's IP address)
//		hConnect = InternetOpenUrlA(hInternet, url.c_str(), NULL, 0, INTERNET_FLAG_RELOAD, 0);
//
//		if (hConnect != NULL) {
//			// Send the POST request
//			success = HttpSendRequestA(hConnect, "POST", 0, NULL, 0);
//
//			// Read the response
//			char buffer[4096];
//			while (InternetReadFile(hConnect, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
//				buffer[bytesRead] = '\0';  // Null terminate the response
//				std::cout << buffer << std::endl;
//			}
//			InternetCloseHandle(hConnect);
//		}
//		InternetCloseHandle(hInternet);
//	}
//
//	return success;
//}

std::string DiscoverHEOSDevice() {
	WSADATA wsaData;
	SOCKET sock;
	sockaddr_in dest;
	char buffer[1024];
	std::string targetIpAddress;

	// Initialize Winsock
	WSAStartup(MAKEWORD(2, 2), &wsaData);
	sock = socket(AF_INET, SOCK_DGRAM, 0);

	// Set socket options for broadcast
	int broadcastEnable = 1;
	setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (char*)&broadcastEnable, sizeof(broadcastEnable));

	// Set non-blocking mode for the socket
	u_long mode = 1;
	ioctlsocket(sock, FIONBIO, &mode);

	// Set up destination address for multicast
	dest.sin_family = AF_INET;
	dest.sin_port = htons(1900);
	inet_pton(AF_INET, "239.255.255.250", &dest.sin_addr);

	// Create and send SSDP discovery request
	std::string ssdpRequest =
		"M-SEARCH * HTTP/1.1\r\n"
		"HOST: 239.255.255.250:1900\r\n"
		"MAN: \"ssdp:discover\"\r\n"
		"MX: 3\r\n"
		"ST: urn:schemas-denon-com:device:ACT-Denon:1\r\n\r\n";

	sendto(sock, ssdpRequest.c_str(), (int)ssdpRequest.size(), 0, (sockaddr*)&dest, sizeof(dest));

	// Create storage for responses
	std::vector<std::pair<std::string, std::string>> deviceResponses; // IP, response content pairs

	// Set timeout for discovery
	auto startTime = std::chrono::steady_clock::now();
	const int DISCOVERY_TIMEOUT_SEC = 5; // Wait for 5 seconds to collect responses

	// Collect responses for the specified time
	while (std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::steady_clock::now() - startTime).count() < DISCOVERY_TIMEOUT_SEC) {

		fd_set readfds;
		FD_ZERO(&readfds);
		FD_SET(sock, &readfds);

		// Short timeout for select to avoid blocking too long
		timeval selectTimeout = { 0, 100000 }; // 100ms

		if (select(0, &readfds, NULL, NULL, &selectTimeout) > 0) {
			sockaddr_in sender;
			int senderLen = sizeof(sender);
			memset(buffer, 0, sizeof(buffer));

			int len = recvfrom(sock, buffer, sizeof(buffer) - 1, 0, (sockaddr*)&sender, &senderLen);
			if (len > 0) {
				buffer[len] = '\0';
				std::string response(buffer);

				char str[INET_ADDRSTRLEN];  // Buffer for the IP address
				if (inet_ntop(AF_INET, &sender.sin_addr, str, sizeof(str)) != NULL) {
					std::string ipAddress = std::string(str);
					// Store the response and IP
					deviceResponses.push_back({ ipAddress, response });
					std::cout << "Received response from " << ipAddress << std::endl;
				}
			}
		}

		// Small sleep to prevent CPU hogging
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	// Clean up socket
	closesocket(sock);
	WSACleanup();

	// Process and filter responses
	for (const auto& device : deviceResponses) {
		const std::string& ipAddress = device.first;
		const std::string& response = device.second;

		// Filter for HEOS devices
		if (response.find("HEOS") != std::string::npos ||
			response.find("Denon") != std::string::npos ||
			response.find("DENON") != std::string::npos) {

			// Check specifically for HEOS Bar if needed
			if (response.find("HEOS Bar") != std::string::npos ||
				response.find("HEOS_Bar") != std::string::npos) {
				std::cout << "Found HEOS Bar at: " << ipAddress << std::endl;
				return ipAddress;
			}

			// Keep track of any HEOS device in case we don't find a specific HEOS Bar
			targetIpAddress = ipAddress;
			std::cout << "Found HEOS device at: " << ipAddress << std::endl;
		}
	}

	// If we found any HEOS device but not specifically a HEOS Bar, return that
	if (!targetIpAddress.empty()) {
		return targetIpAddress;
	}

	// If we found nothing at all
	std::cout << "No HEOS devices found." << std::endl;
	return "";
}

std::vector<HeosPlayer> GetHeosPlayers(const std::string& ip) {
	std::vector<HeosPlayer> players;
	WSADATA wsaData;
	WSAStartup(MAKEWORD(2, 2), &wsaData);

	SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in server;
	server.sin_family = AF_INET;
	server.sin_port = htons(1255);
	inet_pton(AF_INET, ip.c_str(), &server.sin_addr);

	if (connect(sock, (sockaddr*)&server, (int)sizeof(server)) == SOCKET_ERROR) {
		closesocket(sock);
		WSACleanup();
		return players;
	}

	std::string command = "heos://player/get_players\r\n";
	send(sock, command.c_str(), (int)command.size(), 0);

	char buffer[8192];
	int bytesReceived = recv(sock, buffer, sizeof(buffer) - 1, 0);
	buffer[bytesReceived] = '\0';

	closesocket(sock);
	WSACleanup();

	std::string response(buffer);
	size_t jsonStart = response.find('{');
	if (jsonStart != std::string::npos) {
		std::string jsonPart = response.substr(jsonStart);
		try {
			Json::Reader reader;
			Json::Value root;
			if (reader.parse(jsonPart, root)) {
				const Json::Value& playersJson = root["payload"];
				for (const auto& item : playersJson) {
					HeosPlayer player;
					player.name = item["name"].asString();
					player.ip = item["ip"].asString();
					player.pid = item["pid"].asString();
					players.push_back(player);
				}
			}
			else {
				std::cerr << "Failed to parse JSON.\n";
			}
		}
		catch (...) {
			std::cerr << "Error during JSON parsing.\n";
		}
	}

	return players;
}

void ChangeTrayIcon(int iconID) {
	HICON hIcon = (HICON)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(iconID), IMAGE_ICON, 0, 0, LR_SHARED);
	nid.hIcon = hIcon;
	nid.uFlags = NIF_ICON; // We're updating the icon only
	Shell_NotifyIcon(NIM_MODIFY, &nid);
}

void SetMutedInternally(bool muted)
{
	isMuted = muted;
	ChangeTrayIcon(isMuted ? IDI_TRAY_MUTED : IDI_TRAY);
}

void GetMuteState(const std::function<void()>& callback)
{
	SendHeosCommand("get_mute", "", [callback](const std::string& response) {
		SetMutedInternally(response.find("state=on") != std::string::npos);
		if (callback) { callback(); }
		});
}

void SetMuteState(bool muted)
{
	SetMutedInternally(muted);
	SendHeosCommand("set_mute", muted ? "state=on" : "state=off");
}

//void GetPlayState()
//{
//	SendHeosCommand("get_play_state", "", [](const std::string& response) {
//		isPlaying = response.find("state=play") != std::string::npos;
//		});
//}
//
//void SetPlayState(bool play)
//{
//	SendHeosCommand("set_play_state", play ? "state=play" : "state=pause");
//}

//void GetInput()
//{
//	SendHeosCommand("get_now_playing_media", "", [](const std::string& response) {
//		currentInput.clear();
//
//		try
//		{
//			Json::Value root;
//			Json::CharReaderBuilder builder;
//			std::string errs;
//			std::istringstream s(response);
//			if (Json::parseFromStream(builder, s, &root, &errs)) {
//				currentInput = ToLowercase(root["payload"]["song"].asString());
//			}
//		}
//		catch (...) { }
//		});
//}

//bool IsOpticalIn()
//{
//	return currentInput.find("optical") != std::string::npos;
//}

void SetInput(const std::string input)
{
	//currentInput = input;
	SendHeosCommand("play_input", "input=inputs/" + input);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
	std::cout.rdbuf(out.rdbuf());  // Redirect all std::cout output
	hInst = hInstance;

	WNDCLASS wc = {};
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInstance;
	wc.lpszClassName = L"TrayIconClass";
	RegisterClass(&wc);

	hwndMain = CreateWindowEx(0, L"TrayIconClass", L"Tray Icon", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);

	HICON hIcon = (HICON)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_TRAY), IMAGE_ICON, 0, 0, LR_SHARED);

	ZeroMemory(&nid, sizeof(nid));
	nid.cbSize = sizeof(nid);
	nid.hWnd = hwndMain;
	nid.uID = TRAY_ICON_UID;
	nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
	nid.uCallbackMessage = WM_TRAYICON;
	//nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
	nid.hIcon = hIcon;
	wcscpy_s(nid.szTip, TRAY_ICON_TOOLTIP);

	Shell_NotifyIcon(NIM_ADD, &nid);

	std::ifstream in(PREFS_FILE);
	if (in) {
		Json::Value root;
		in >> root;

		deviceIP = root.get("ip", "").asString();
		devicePID = root.get("pid", "").asString();
		deviceName = root.get("name", "Not connected").asString();
	}

	if (!deviceIP.empty())
	{
		GetMuteState(NULL);
		//GetPlayState();
		//GetInput();
	}

	ValidateConnection();

	//std::thread slackMonitorLoop(SlackMonitorLoop);

	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	Shell_NotifyIcon(NIM_DELETE, &nid);
	return 0;
}

void ShowContextMenu(HWND hwnd, POINT pt)
{
	HMENU hMenu = CreatePopupMenu();
	if (!hMenu) return;

	bool startupEnabled = IsStartupEnabled();

	std::wstring info = isConnected ? L"Connected to " + std::wstring(deviceName.begin(), deviceName.end()) + L" (" + std::wstring(deviceIP.begin(), deviceIP.end()) + L")" : L"Not connected";
	AppendMenu(hMenu, MF_STRING | MF_DISABLED, ID_TRAY_DEVICE_INFO, info.c_str());
	AppendMenu(hMenu, MF_STRING, ID_TRAY_CONNECT, L"Connect!");
	AppendMenu(hMenu, MF_STRING | (startupEnabled ? MF_CHECKED : 0), ID_TRAY_STARTUP, L"Launch on startup");
	AppendMenu(hMenu, MF_STRING, ID_TRAY_EXIT, L"Quit");

	SetForegroundWindow(hwnd);
	TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
	DestroyMenu(hMenu);
}

#define ARRAY_LENGTH(arr) (sizeof(arr) / sizeof((arr)[0]))
int buttonIDs[] = { ID_BUTTON_PLAY_PAUSE, ID_BUTTON_MUTE, ID_BUTTON_VOL_DOWN, ID_BUTTON_VOL_UP, ID_BUTTON_OPTICAL };
#define BUTTON_COUNT ARRAY_LENGTH(buttonIDs)
#define ICO_SIZE 64
#define BUTTON_SIZE 32
#define MARGIN 32

//HICON LoadButtonIcon(LPCWSTR iconPath)
HICON LoadButtonIcon(int iconID)
{
	// Load a large icon and scale it down to fit 32x32 using an image list
	HICON hIconLarge = (HICON)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(iconID), IMAGE_ICON, ICO_SIZE, ICO_SIZE, LR_SHARED);
	if (!hIconLarge) return NULL;

	// Create image list with 32x32 size
	HIMAGELIST hImageList = ImageList_Create(BUTTON_SIZE, BUTTON_SIZE, ILC_COLOR32 | ILC_MASK, 1, 1);
	if (!hImageList) return hIconLarge;

	ImageList_AddIcon(hImageList, hIconLarge);
	HICON hIconSmall = ImageList_GetIcon(hImageList, 0, ILD_NORMAL);

	ImageList_Destroy(hImageList);
	DestroyIcon(hIconLarge);

	return hIconSmall;
}


void ShowButtonToolbar()
{
	if (hwndToolbar != NULL)
		return; // Already shown

	RECT workArea;
	SystemParametersInfo(SPI_GETWORKAREA, 0, &workArea, 0);

	hwndToolbar = CreateWindowEx(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"STATIC", NULL,
		WS_POPUP | WS_VISIBLE,
		workArea.right - BUTTON_SIZE * BUTTON_COUNT - MARGIN, workArea.bottom - BUTTON_SIZE - MARGIN,
		BUTTON_SIZE * BUTTON_COUNT, BUTTON_SIZE,
		NULL, NULL, hInst, NULL);

	HWND buttons[BUTTON_COUNT];
	//LPCWSTR icons[] = { L"play.ico", L"pause.ico", L"mute.ico", L"voldown.ico", L"volup.ico", L"optical.ico" };

	for (int i = 0; i < BUTTON_COUNT; ++i) {
		int iconID = buttonIDs[i] - ID_BUTTON_PLAY_PAUSE + IDI_BUTTON_PLAY;
		//HICON hIcon = LoadButtonIcon(icons[i]);
		HICON hIcon = LoadButtonIcon(iconID);
		buttons[i] = CreateWindow(L"BUTTON", NULL,
			WS_CHILD | WS_VISIBLE | BS_ICON,
			i * BUTTON_SIZE, 0, BUTTON_SIZE, BUTTON_SIZE,
			hwndToolbar, (HMENU)(INT_PTR)buttonIDs[i], hInst, NULL);
		SendMessage(buttons[i], BM_SETIMAGE, IMAGE_ICON, (LPARAM)hIcon);
	}

	ShowWindow(hwndToolbar, SW_SHOWNOACTIVATE);
	SetForegroundWindow(hwndToolbar);
	SetFocus(hwndToolbar);

	// Track whether we're processing a button click
	static bool processingButtonClick = false;

	// Auto-hide on focus loss, but not when clicking buttons
	SetWindowLongPtr(hwndToolbar, GWLP_WNDPROC, (LONG_PTR)+[](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
		switch (msg) {
		case WM_COMMAND:
			// Set flag when processing button clicks
			processingButtonClick = true;
			break;

		case WM_ACTIVATE:
			if (LOWORD(wParam) == WA_INACTIVE)
			{
				// Check if we're just clicking a button in our toolbar
				HWND hwndFocus = GetFocus();
				HWND hwndActive = (HWND)lParam;

				// If we're processing a button click or the active window is one of our buttons, don't close
				if (processingButtonClick || IsChild(hwnd, hwndActive))
				{
					processingButtonClick = false;
					return 0;
				}

				// Otherwise close the toolbar
				ShowWindow(hwndToolbar, SW_HIDE);
				DestroyWindow(hwndToolbar);
				hwndToolbar = NULL;
				return 0;
			}
			break;
		}
		processingButtonClick = false;
		return WndProc(hwnd, msg, wParam, lParam);
		});
}

void ValidateConnection()
{
	std::thread([] {
		std::string ip = DiscoverHEOSDevice();
		if (ip.empty()) {
			std::cout << "No HEOS device found.\n";
			return;
		}

		std::cout << "Discovered HEOS IP: " << ip << "\n";
		auto players = GetHeosPlayers(ip);

		isConnected = false;
		deviceName = "Not connected";
		deviceIP.clear();
		devicePID.clear();

		for (const auto& player : players) {
			std::cout << "Player: " << player.name << " at " << player.ip << " = " + player.pid + "\n";
			isConnected = true;
			deviceName = players[0].name;
			deviceIP = players[0].ip;
			devicePID = players[0].pid;
		}

		Json::Value root;
		{
			root["ip"] = deviceIP;
			root["pid"] = devicePID;
			root["name"] = deviceName;
		}
		std::ofstream out(PREFS_FILE);
		out << root;

		GetMuteState(NULL);
		}).detach();
}

void ToggleMute()
{
	GetMuteState([]() { SetMuteState(!isMuted); });
}

// Check if any window title includes "Slack (Screen Sharing)"
bool IsSlackScreenSharing() {
	HWND hwnd = nullptr;
	wchar_t title[512];
	while ((hwnd = FindWindowEx(nullptr, hwnd, nullptr, nullptr)) != nullptr) {
		if (!IsWindowVisible(hwnd)) continue;
		GetWindowTextW(hwnd, title, 512);
		std::wstring wTitle(title);
		if (wTitle.find(L"Slack") != std::wstring::npos &&
			wTitle.find(L"Screen Sharing") != std::wstring::npos) {
			return true;
		}
	}
	return false;
}

void SlackMonitorLoop() {
	bool wasSharing = false;
	bool hasMuted = false;
	while (true) {
		bool isSharing = IsSlackScreenSharing();
		if (isSharing && !wasSharing) {
			std::cout << "[Slack] Detected screen sharing — muting HEOS\n";
			GetMuteState([&hasMuted]() {
				if (!isMuted)
				{
					hasMuted = true;
					SetMuteState(true);
				}
			});
		}
		else if (!isSharing && wasSharing) {
			std::cout << "[Slack] Screen sharing stopped — checking mute status\n";
			if (hasMuted) {
				std::cout << "[Slack] Unmuting HEOS\n";
				hasMuted = false;
				SetMuteState(false);
			}
			else {
				std::cout << "[Slack] Was already muted — skipping unmute\n";
			}
		}

		wasSharing = isSharing;
		std::this_thread::sleep_for(std::chrono::seconds(2));
	}
}

static UINT_PTR clickTimerID = 0;
static bool waitingForDoubleClick = false;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg) {
	case WM_TRAYICON:
		switch (lParam) {
		case WM_LBUTTONDOWN:
			if (!waitingForDoubleClick) {
				waitingForDoubleClick = true;
				auto dblClickTime = 100;//GetDoubleClickTime(); // 100 because the default is 500 and that is just too long.
				clickTimerID = SetTimer(hwnd, 1, 100, NULL);
			}
			break;

		case WM_LBUTTONDBLCLK:
			if (clickTimerID) {
				KillTimer(hwnd, clickTimerID);
				clickTimerID = 0;
			}
			waitingForDoubleClick = false;

			ToggleMute();
			break;

		case WM_RBUTTONUP:
		{
			POINT pt;
			GetCursorPos(&pt);
			ShowContextMenu(hwnd, pt);
			break;
		}
		}
		break;

	case WM_TIMER:
		if (wParam == clickTimerID) {
			// Timer expired without a double-click, so process as single click
			KillTimer(hwnd, clickTimerID);
			clickTimerID = 0;
			waitingForDoubleClick = false;

			// Now it's safe to show the toolbar
			ShowButtonToolbar();
		}
		break;

	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case ID_TRAY_EXIT:
			PostQuitMessage(0);
			break;
		case ID_TRAY_CONNECT:
			ValidateConnection();
			break;
		case ID_TRAY_STARTUP:
		{
			BOOL checked = SendMessage((HWND)lParam, BM_GETCHECK, 0, 0) == BST_CHECKED;
			if (checked)
				AddAppToStartup();
			else
				RemoveAppFromStartup();
		}
		break;
		case ID_BUTTON_PLAY_PAUSE:
			//GetInput();
			//if (IsOpticalIn())
			{
				keybd_event(VK_MEDIA_PLAY_PAUSE, 0, KEYEVENTF_EXTENDEDKEY, 0);
			}
			//else
			//{
			//	SetPlayState(!isPlaying);
			//}
			break;
		case ID_BUTTON_MUTE:
			ToggleMute();
			break;
		case ID_BUTTON_VOL_DOWN:
			SetMutedInternally(false);
			SendHeosCommand("volume_down");
			break;
		case ID_BUTTON_VOL_UP:
			SetMutedInternally(false);
			SendHeosCommand("volume_up");
			break;
		case ID_BUTTON_OPTICAL:
			SetInput("optical_in_1");
			break;
		}
		break;

	case WM_DESTROY:
		if (hwnd == hwndMain)
		{
			PostQuitMessage(0);
		}
		break;

	default:
		return DefWindowProc(hwnd, msg, wParam, lParam);
	}
	return 0;
}
