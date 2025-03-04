#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <filesystem>
#include <sstream>
#include <fstream>
#include <shlwapi.h>
#include <iostream>

#pragma comment(lib, "Ws2_32.lib")

#include <winsock2.h>
#include <WS2tcpip.h>

namespace fs = std::filesystem;

extern "C"
{
	__declspec(dllexport) DWORD AmdPowerXpressRequestHighPerformance = 0x00000001;
	__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
}

HMODULE hLauncherModule;
HMODULE hHookModule;
HMODULE hTier0Module;

wchar_t exePath[4096];
wchar_t buffer[8192];

bool noLoadPlugins = false;

DWORD GetProcessByName(std::wstring processName)
{
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

	PROCESSENTRY32 processSnapshotEntry = {0};
	processSnapshotEntry.dwSize = sizeof(PROCESSENTRY32);

	if (snapshot == INVALID_HANDLE_VALUE)
		return 0;

	if (!Process32First(snapshot, &processSnapshotEntry))
		return 0;

	while (Process32Next(snapshot, &processSnapshotEntry))
	{
		if (!wcscmp(processSnapshotEntry.szExeFile, processName.c_str()))
		{
			CloseHandle(snapshot);
			return processSnapshotEntry.th32ProcessID;
		}
	}

	CloseHandle(snapshot);
	return 0;
}

bool GetExePathWide(wchar_t* dest, DWORD destSize)
{
	if (!dest)
		return NULL;
	if (destSize < MAX_PATH)
		return NULL;

	DWORD length = GetModuleFileNameW(NULL, dest, destSize);
	return length && PathRemoveFileSpecW(dest);
}

FARPROC GetLauncherMain()
{
	static FARPROC Launcher_LauncherMain;
	if (!Launcher_LauncherMain)
		Launcher_LauncherMain = GetProcAddress(hLauncherModule, "LauncherMain");
	return Launcher_LauncherMain;
}

void LibraryLoadError(DWORD dwMessageId, const wchar_t* libName, const wchar_t* location)
{
	char text[8192];
	std::string message = std::system_category().message(dwMessageId);

	sprintf_s(
		text,
		"Failed to load the %ls at \"%ls\" (%lu):\n\n%hs\n\nMake sure you followed the Ronin installation instructions carefully "
		"before reaching out for help.",
		libName,
		location,
		dwMessageId,
		message.c_str());

	if (dwMessageId == 126 && std::filesystem::exists(location))
	{
		sprintf_s(
			text,
			"%s\n\nThe file at the specified location DOES exist, so this error indicates that one of its *dependencies* failed to be "
			"found.\n\nTry the following steps:\n1. Install Visual C++ 2022 Redistributable: "
			"https://aka.ms/vs/17/release/vc_redist.x64.exe\n2. Repair game files",
			text);
	}
	else if (!fs::exists("Titanfall2.exe") && (fs::exists("..\\Titanfall2.exe") || fs::exists("..\\..\\Titanfall2.exe")))
	{
		auto curDir = std::filesystem::current_path().filename().string();
		auto aboveDir = std::filesystem::current_path().parent_path().filename().string();
		sprintf_s(
			text,
			"%s\n\nWe detected that in your case you have extracted the files into a *subdirectory* of your Titanfall 2 "
			"installation.\nPlease move all the files and folders from current folder (\"%s\") into the Titanfall 2 installation directory "
			"just above (\"%s\").\n\nPlease try out the above steps by yourself before reaching out to the community for support.",
			text,
			curDir.c_str(),
			aboveDir.c_str());
	}
	else if (!fs::exists("Titanfall2.exe"))
	{
		sprintf_s(
			text,
			"%s\n\nRemember: you need to unpack the contents of this archive into your Titanfall 2 game installation directory, not just "
			"to any random folder.",
			text);
	}
	else if (fs::exists("Titanfall2.exe"))
	{
		sprintf_s(
			text,
			"%s\n\nTitanfall2.exe has been found in the current directory: is the game installation corrupted or did you not unpack all "
			"Ronin files here?",
			text);
	}

	MessageBoxA(GetForegroundWindow(), text, "Ronin Launcher Error", 0);
}

void AwaitOriginStartup()
{
	WSADATA wsaData;
	WSAStartup(MAKEWORD(2, 2), &wsaData);
	SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (sock != INVALID_SOCKET)
	{
		const int LSX_PORT = 3216;

		sockaddr_in lsxAddr;
		lsxAddr.sin_family = AF_INET;
		inet_pton(AF_INET, "127.0.0.1", &(lsxAddr.sin_addr));
		lsxAddr.sin_port = htons(LSX_PORT);

		std::cout << "LSX: connect()" << std::endl;
		connect(sock, (struct sockaddr*)&lsxAddr, sizeof(lsxAddr));

		char buf[4096];
		memset(buf, 0, sizeof(buf));

		do
		{
			recv(sock, buf, 4096, 0);
			std::cout << buf << std::endl;

			// honestly really shit, this isn't needed for origin due to being able to check OriginClientService
			// but for ea desktop we don't have anything like this, so atm we just have to wait to ensure that we start after logging in
			Sleep(8000);
		} while (!strstr(buf, "<LSX>")); // ensure we're actually getting data from lsx
	}

	WSACleanup(); // cleanup sockets and such so game can contact lsx itself
}

void EnsureOriginStarted()
{
	if (GetProcessByName(L"Origin.exe") || GetProcessByName(L"EADesktop.exe"))
		return; // already started

	// unpacked exe will crash if origin isn't open on launch, so launch it
	// get origin path from registry, code here is reversed from OriginSDK.dll
	HKEY key;
	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Origin", 0, KEY_READ, &key) != ERROR_SUCCESS)
	{
		MessageBoxA(0, "Error: failed reading Origin path!", "Ronin Launcher Error", MB_OK);
		return;
	}

	char originPath[520];
	DWORD originPathLength = 520;
	if (RegQueryValueExA(key, "ClientPath", 0, 0, (LPBYTE)&originPath, &originPathLength) != ERROR_SUCCESS)
	{
		MessageBoxA(0, "Error: failed reading Origin path!", "Ronin Launcher Error", MB_OK);
		return;
	}

	std::cout << "[*] Starting Origin..." << std::endl;

	PROCESS_INFORMATION pi;
	memset(&pi, 0, sizeof(pi));
	STARTUPINFO si;
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(STARTUPINFO);
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_MINIMIZE;
	CreateProcessA(
		originPath,
		(char*)"",
		NULL,
		NULL,
		false,
		CREATE_DEFAULT_ERROR_MODE | CREATE_NEW_PROCESS_GROUP,
		NULL,
		NULL,
		(LPSTARTUPINFOA)&si,
		&pi);

	std::cout << "[*] Waiting for Origin..." << std::endl;

	// wait for origin process to boot
	do
	{
		Sleep(500);
	} while (!GetProcessByName(L"OriginClientService.exe") && !GetProcessByName(L"EADesktop.exe"));

	// wait for origin to be ready to start
	AwaitOriginStartup();

	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
}

void PrependPath()
{
	wchar_t* pPath;
	size_t len;
	errno_t err = _wdupenv_s(&pPath, &len, L"PATH");
	if (!err)
	{
		swprintf_s(buffer, L"PATH=%s\\bin\\x64_retail\\;%s", exePath, pPath);
		auto result = _wputenv(buffer);
		if (result == -1)
		{
			MessageBoxW(
				GetForegroundWindow(),
				L"Warning: could not prepend the current directory to app's PATH environment variable. Something may break because of "
				L"that.",
				L"Ronin Launcher Warning",
				0);
		}
		free(pPath);
	}
	else
	{
		MessageBoxW(
			GetForegroundWindow(),
			L"Warning: could not get current PATH environment variable in order to prepend the current directory to it. Something may "
			L"break because of that.",
			L"Ronin Launcher Warning",
			0);
	}
}

bool ShouldLoadRonin(int argc, char* argv[])
{
	bool loadRonin = true;
	for (int i = 0; i < argc; i++)
		if (!strcmp(argv[i], "-vanilla"))
			loadRonin = false;

	if (!loadRonin)
		return loadRonin;

	auto runRoninFile = std::ifstream("run_ronin.txt");
	if (runRoninFile)
	{
		std::stringstream runRoninFileBuffer;
		runRoninFileBuffer << runRoninFile.rdbuf();
		runRoninFile.close();
		if (runRoninFileBuffer.str().starts_with("0"))
			loadRonin = false;
	}
	return loadRonin;
}

bool LoadRonin()
{
	FARPROC Hook_Init = nullptr;
	{
		swprintf_s(buffer, L"%s\\Ronin.dll", exePath);
		hHookModule = LoadLibraryExW(buffer, 0, 8u);
		if (hHookModule)
			Hook_Init = GetProcAddress(hHookModule, "InitialiseRonin");
		if (!hHookModule || Hook_Init == nullptr)
		{
			LibraryLoadError(GetLastError(), L"Ronin.dll", buffer);
			return false;
		}
	}
	((bool (*)())Hook_Init)();

	FARPROC LoadPlugins = nullptr;
	if (!noLoadPlugins)
	{
		LoadPlugins = GetProcAddress(hHookModule, "LoadPlugins");
		if (!hHookModule || LoadPlugins == nullptr)
		{
			std::cout << "Failed to get function pointer to LoadPlugins of Ronin.dll" << std::endl;
			LibraryLoadError(GetLastError(), L"Ronin.dll", buffer);
			return false;
		}
		((bool (*)())LoadPlugins)();
	}

	return true;
}

HMODULE LoadDediStub(const char* name)
{
	// this works because materialsystem_dx11.dll uses relative imports, and even a DLL loaded with an absolute path will take precedence
	std::cout << "[*]   Loading " << name << std::endl;
	swprintf_s(buffer, L"%s\\bin\\x64_dedi\\%hs", exePath, name);
	HMODULE h = LoadLibraryExW(buffer, 0, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!h)
	{
		wprintf(L"[*] Failed to load stub %hs from \"%ls\": %hs\n", name, buffer, std::system_category().message(GetLastError()).c_str());
	}
	return h;
}

int main(int argc, char* argv[])
{

	if (strstr(GetCommandLineA(), "-waitfordebugger"))
	{
		while (!IsDebuggerPresent())
		{
			// Sleep 100ms to give debugger time to attach.
			Sleep(100);
		}
	}

	if (!GetExePathWide(exePath, sizeof(exePath)))
	{
		MessageBoxA(
			GetForegroundWindow(),
			"Failed getting game directory.\nThe game cannot continue and has to exit.",
			"Ronin Launcher Error",
			0);
		return 1;
	}

	SetCurrentDirectoryW(exePath);

	bool noOriginStartup = false;
	bool dedicated = false;
	bool nostubs = false;

	for (int i = 0; i < argc; i++)
		if (!strcmp(argv[i], "-noOriginStartup"))
			noOriginStartup = true;
		else if (!strcmp(argv[i], "-dedicated")) // also checked by Ronin.dll
			dedicated = true;
		else if (!strcmp(argv[i], "-nostubs"))
			nostubs = true;
		else if (!strcmp(argv[i], "-noplugins"))
			noLoadPlugins = true;

	if (!noOriginStartup && !dedicated)
	{
		EnsureOriginStarted();
	}

	if (dedicated && !nostubs)
	{
		std::cout << "[*] Loading stubs" << std::endl;
		HMODULE gssao, gtxaa, d3d11;
		if (!(gssao = GetModuleHandleA("GFSDK_SSAO.win64.dll")) && !(gtxaa = GetModuleHandleA("GFSDK_TXAA.win64.dll")) &&
			!(d3d11 = GetModuleHandleA("d3d11.dll")))
		{
			if (!(gssao = LoadDediStub("GFSDK_SSAO.win64.dll")) || !(gtxaa = LoadDediStub("GFSDK_TXAA.win64.dll")) ||
				!(d3d11 = LoadDediStub("d3d11.dll")))
			{
				if ((!gssao || FreeLibrary(gssao)) && (!gtxaa || FreeLibrary(gtxaa)) && (!d3d11 || FreeLibrary(d3d11)))
				{
					std::cout << "[*] WARNING: Failed to load d3d11/gfsdk stubs from bin/x64_dedi. "
								 "The stubs have been unloaded and the original libraries will be used instead"
							  << std::endl;
				}
				else
				{
					// this is highly unlikely
					MessageBoxA(
						GetForegroundWindow(),
						"Failed to load one or more stubs, but could not unload them either.\n"
						"The game cannot continue and has to exit.",
						"Ronin Launcher Error",
						0);
					return 1;
				}
			}
		}
		else
		{
			// this should never happen
			std::cout << "[*] WARNING: Failed to load stubs because conflicting modules are already loaded, so those will be used instead "
						 "(did Ronin initialize too late?)."
					  << std::endl;
		}
	}

	{
		PrependPath();

		if (!fs::exists("rn_startup_args.txt"))
		{
			std::ofstream file("rn_startup_args.txt");
			std::string defaultArgs = "-novid";
			file.write(defaultArgs.c_str(), defaultArgs.length());
			file.close();
		}

		std::cout << "[*] Loading tier0.dll" << std::endl;
		swprintf_s(buffer, L"%s\\bin\\x64_retail\\tier0.dll", exePath);
		hTier0Module = LoadLibraryExW(buffer, 0, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (!hTier0Module)
		{
			LibraryLoadError(GetLastError(), L"tier0.dll", buffer);
			return 1;
		}

		bool loadRonin = ShouldLoadRonin(argc, argv);
		if (loadRonin)
		{
			std::cout << "[*] Loading Ronin" << std::endl;
			if (!LoadRonin())
				return 1;
		}
		else
			std::cout << "[*] Going to load the vanilla game" << std::endl;

		std::cout << "[*] Loading launcher.dll" << std::endl;
		swprintf_s(buffer, L"%s\\bin\\x64_retail\\launcher.dll", exePath);
		hLauncherModule = LoadLibraryExW(buffer, 0, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (!hLauncherModule)
		{
			LibraryLoadError(GetLastError(), L"launcher.dll", buffer);
			return 1;
		}
	}

	std::cout << "[*] Launching the game..." << std::endl;
	auto LauncherMain = GetLauncherMain();
	if (!LauncherMain)
		MessageBoxA(
			GetForegroundWindow(),
			"Failed loading launcher.dll.\nThe game cannot continue and has to exit.",
			"Ronin Launcher Error",
			0);

	std::cout.flush();
	return ((int(/*__fastcall*/*)(HINSTANCE, HINSTANCE, LPSTR, int))LauncherMain)(
		NULL, NULL, NULL, 0); // the parameters aren't really used anyways
}
