// Pons' automated tests: a minimal runner without dependencies.
//
//   PonsTests.exe
//
// The server tests run a real server on 127.0.0.1 and talk to it over sockets.

#include "core/http.h"
#include "core/qr.h"
#include "core/server.h"
#include "core/settings.h"
#include "core/text.h"
#include "core/web_page.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

using namespace pons;
namespace fs = std::filesystem;

namespace {

struct TestCase {
    const char* name;
    std::function<void()> run;
};

std::vector<TestCase>& Tests() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> run) { Tests().push_back({name, std::move(run)}); }
};

struct Failure {
    std::string message;
};

#define TEST(name)                                  \
    static void name();                             \
    static Registrar name##_registrar(#name, name); \
    static void name()

#define CHECK(expression)                                                                                              \
    do {                                                                                                               \
        if (!(expression)) throw Failure{std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " + #expression}; \
    } while (false)

constexpr wchar_t kTestSettingsKey[] = L"Software\\Pons WiFi Transfer Tests";
constexpr uint32_t kLoopback = 0x7F000001;

fs::path TemporaryFolder() {
    wchar_t folder[MAX_PATH];
    GetTempPathW(MAX_PATH, folder);
    const fs::path path = fs::path(folder) / L"PonsTests";
    fs::create_directories(path);
    return path;
}

struct Response {
    int status = 0;
    std::string head;
    std::string body;
};

// One request on a fresh connection, read to the end.
Response Request(int port, const std::string& request) {
    Response response;
    SOCKET connection = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(port));
    address.sin_addr.s_addr = htonl(kLoopback);
    if (connect(connection, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        closesocket(connection);
        return response;
    }
    send(connection, request.data(), static_cast<int>(request.size()), 0);
    shutdown(connection, SD_SEND);
    std::string data;
    char buffer[8192];
    int received;
    while ((received = recv(connection, buffer, sizeof(buffer), 0)) > 0) data.append(buffer, static_cast<size_t>(received));
    closesocket(connection);
    const size_t headEnd = data.find("\r\n\r\n");
    if (headEnd == std::string::npos) return response;
    response.head = data.substr(0, headEnd);
    response.body = data.substr(headEnd + 4);
    if (response.head.size() > 12) response.status = std::stoi(response.head.substr(9, 3));
    return response;
}

std::string Get(const std::string& target) { return "GET " + target + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"; }

std::string Post(const std::string& target, const std::string& body) {
    return "POST " + target + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\n\r\n" + body;
}

struct TestServer {
    Server server;
    fs::path folder;
    std::string token;

    TestServer() {
        folder = TemporaryFolder() / L"received";
        fs::remove_all(folder);
        ServerOptions options;
        options.address = kLoopback;
        options.port = 18642;
        options.receiveFolder = folder;
        options.allowLoopback = true;
        if (!server.Start(options)) throw Failure{"server did not start"};
        token = server.Token();
    }
    int port() const { return server.Port(); }
};

// --- HTTP pieces ------------------------------------------------------------------

TEST(ParsesRequestHead) {
    const auto request = ParseRequestHead("POST /api/upload?t=abc&name=a%20b.jpg HTTP/1.1\r\nHost: x\r\ncontent-length: 42\r\n");
    CHECK(request);
    CHECK(request->method == "POST");
    CHECK(request->path == "/api/upload");
    CHECK(request->query == "t=abc&name=a%20b.jpg");
    CHECK(request->hasContentLength && request->contentLength == 42);
    CHECK(request->keepAlive);
    CHECK(!ParseRequestHead("GET /x HTTP/1.0\r\n")->keepAlive);
    CHECK(!ParseRequestHead("GET / HTTP/1.1\r\nConnection: close")->keepAlive);
}

TEST(RejectsMalformedRequests) {
    CHECK(!ParseRequestHead("garbage"));
    CHECK(!ParseRequestHead("GET noslash HTTP/1.1"));
    CHECK(!ParseRequestHead("GET / FTP/1.0"));
    CHECK(!ParseRequestHead("POST / HTTP/1.1\r\nContent-Length: 12x"));
    CHECK(!ParseRequestHead("POST / HTTP/1.1\r\nContent-Length: -1"));
    CHECK(!ParseRequestHead("POST / HTTP/1.1\r\nTransfer-Encoding: chunked"));
}

TEST(DecodesQueries) {
    CHECK(UrlDecode("a%20b+c") == "a b c");
    CHECK(UrlDecode("a+b", false) == "a+b");
    CHECK(UrlDecode("100%") == "100%");
    CHECK(UrlDecode("%zz") == "%zz");
    CHECK(QueryParam("t=abc&name=Fot%C3%B3.jpg", "name") == "Fot\xC3\xB3.jpg");
    CHECK(QueryParam("t=abc", "t") == "abc");
    CHECK(!QueryParam("tt=abc", "t"));
    CHECK(QueryParam("flag", "flag") == "");
    CHECK(UrlEncode("a b/\xC3\xB3") == "a%20b%2F%C3%B3");
}

TEST(EscapesJson) {
    CHECK(JsonEscape("a\"b\\c\nd") == "a\\\"b\\\\c\\nd");
    CHECK(JsonEscape("</script>") == "\\u003c/script>");
    CHECK(JsonEscape(std::string("\x01", 1)) == "\\u0001");
}

TEST(SanitizesFileNames) {
    CHECK(SanitizeFileName(L"photo.jpg") == L"photo.jpg");
    CHECK(SanitizeFileName(L"..\\..\\Windows\\evil.exe") == L"evil.exe");
    CHECK(SanitizeFileName(L"/sdcard/DCIM/IMG_1.jpg") == L"IMG_1.jpg");
    CHECK(SanitizeFileName(L"a<b>c:d\"e|f?g*h.txt") == L"a_b_c_d_e_f_g_h.txt");
    CHECK(SanitizeFileName(L"") == L"file");
    CHECK(SanitizeFileName(L"..") == L"file");
    CHECK(SanitizeFileName(L"name. . ") == L"name");
    CHECK(SanitizeFileName(L"CON") == L"_CON");
    CHECK(SanitizeFileName(L"com1.txt") == L"_com1.txt");
    CHECK(SanitizeFileName(L"console.txt") == L"console.txt");
    CHECK(SanitizeFileName(L"Fotó 日本.png") == L"Fotó 日本.png");
    const std::wstring longName = SanitizeFileName(std::wstring(300, L'x') + L".jpeg");
    CHECK(longName.size() == 200 && longName.ends_with(L".jpeg"));
}

TEST(FindsUniquePaths) {
    const fs::path folder = TemporaryFolder() / L"unique";
    fs::remove_all(folder);
    fs::create_directories(folder);
    CHECK(UniquePath(folder, L"a.txt") == folder / L"a.txt");
    std::ofstream(folder / L"a.txt") << "x";
    CHECK(UniquePath(folder, L"a.txt") == folder / L"a (2).txt");
    std::ofstream(folder / L"a (2).txt") << "x";
    CHECK(UniquePath(folder, L"a.txt") == folder / L"a (3).txt");
    fs::remove_all(folder);
}

TEST(BuildsAttachmentHeaders) {
    CHECK(AttachmentHeader(L"a b.txt") == "attachment; filename=\"a b.txt\"; filename*=UTF-8''a%20b.txt");
    CHECK(AttachmentHeader(L"\"ó\".txt") == "attachment; filename=\"___.txt\"; filename*=UTF-8''%22%C3%B3%22.txt");
}

TEST(GeneratesTokens) {
    const std::string a = GenerateToken();
    const std::string b = GenerateToken();
    CHECK(a.size() == 32 && b.size() == 32);
    CHECK(a != b);
    CHECK(a.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK(TokensEqual(a, a));
    CHECK(!TokensEqual(a, b));
    CHECK(!TokensEqual(a, a.substr(1)));
    CHECK(!TokensEqual("", ""));
}

TEST(RecognizesPrivateAddresses) {
    CHECK(IsPrivateIPv4(0x0A000001));   // 10.0.0.1
    CHECK(IsPrivateIPv4(0xAC100001));   // 172.16.0.1
    CHECK(IsPrivateIPv4(0xAC1FFFFF));   // 172.31.255.255
    CHECK(!IsPrivateIPv4(0xAC200001));  // 172.32.0.1
    CHECK(IsPrivateIPv4(0xC0A80114));   // 192.168.1.20
    CHECK(IsPrivateIPv4(0xA9FE0001));   // 169.254.0.1
    CHECK(!IsPrivateIPv4(0x08080808));  // 8.8.8.8
    CHECK(!IsPrivateIPv4(kLoopback));
    CHECK(FormatIPv4(0xC0A80114) == L"192.168.1.20");
}

TEST(FormatsSizes) {
    CHECK(FormatSize(0) == L"0 B");
    CHECK(FormatSize(1023) == L"1023 B");
    CHECK(FormatSize(1536) == L"1.5 KB");
    CHECK(FormatSize(50ull * 1024 * 1024) == L"50 MB");
}

TEST(EncodesQrCodes) {
    const auto qr = EncodeQrCode(L"http://192.168.1.20:8642/?t=0123456789abcdef0123456789abcdef");
    CHECK(qr && qr->size >= 21 && qr->size <= 177);
    CHECK(qr->IsDark(0, 0) && qr->IsDark(6, 6) && !qr->IsDark(7, 7));  // finder pattern
    CHECK(!EncodeQrCode(L""));
}

TEST(PageIsSelfContained) {
    const std::string& page = PageHtml();
    CHECK(page.starts_with("<!doctype html>"));
    CHECK(page.ends_with("</html>\n"));
    CHECK(page.find("http://") == std::string::npos && page.find("https://") == std::string::npos);
}

TEST(SavesSettings) {
    RegDeleteTreeW(HKEY_CURRENT_USER, kTestSettingsKey);
    const Settings defaults = Settings::Load(kTestSettingsKey);
    CHECK(defaults.port == kDefaultPort && defaults.timeoutMinutes == 15 && defaults.receiveFolder.empty());
    Settings settings;
    settings.port = 9000;
    settings.timeoutMinutes = 0;
    settings.receiveFolder = L"D:\\Fotos móvil";
    CHECK(settings.Save(kTestSettingsKey));
    const Settings loaded = Settings::Load(kTestSettingsKey);
    CHECK(loaded.port == 9000 && loaded.timeoutMinutes == 0 && loaded.receiveFolder == settings.receiveFolder);
    RegSetKeyValueW(HKEY_CURRENT_USER, kTestSettingsKey, L"Port", REG_DWORD, "\x10\0\0\0", 4);
    CHECK(Settings::Load(kTestSettingsKey).port == kDefaultPort);
    RegDeleteTreeW(HKEY_CURRENT_USER, kTestSettingsKey);
    CHECK(!IsValidPort(80) && IsValidPort(8642) && !IsValidPort(70000));
}

// --- Server -----------------------------------------------------------------------

TEST(ServerRequiresTheToken) {
    TestServer test;
    CHECK(Request(test.port(), Get("/")).status == 403);
    CHECK(Request(test.port(), Get("/?t=wrong")).status == 403);
    CHECK(Request(test.port(), Get("/api/state")).status == 403);
    CHECK(Request(test.port(), Post("/api/upload?name=x.txt", "data")).status == 403);
    CHECK(!fs::exists(test.folder / L"x.txt"));
    const Response page = Request(test.port(), Get("/?t=" + test.token));
    CHECK(page.status == 200);
    CHECK(page.body == PageHtml());
    CHECK(page.head.find("Content-Security-Policy") != std::string::npos);
    CHECK(page.head.find("Referrer-Policy: no-referrer") != std::string::npos);
}

TEST(ServerReceivesUploads) {
    TestServer test;
    const std::string content(700 * 1024 + 3, 'p');
    const Response first = Request(test.port(), Post("/api/upload?t=" + test.token + "&name=Fot%C3%B3%201.jpg", content));
    CHECK(first.status == 200);
    CHECK(first.body.find("Fot\xC3\xB3 1.jpg") != std::string::npos);
    const Response second = Request(test.port(), Post("/api/upload?t=" + test.token + "&name=..%2F..%2FFot%C3%B3%201.jpg", "x"));
    CHECK(second.status == 200);
    std::ifstream saved(test.folder / L"Fotó 1.jpg", std::ios::binary);
    CHECK(std::string(std::istreambuf_iterator<char>(saved), {}) == content);
    CHECK(fs::file_size(test.folder / L"Fotó 1 (2).jpg") == 1);
    const auto transfers = test.server.Transfers();
    CHECK(transfers.size() == 2);
    CHECK(transfers[0].state == TransferState::Done && transfers[0].done == content.size());
    CHECK(transfers[0].direction == Direction::FromPhone);
    saved.close();
    fs::remove_all(test.folder);
}

TEST(ServerDropsIncompleteUploads) {
    TestServer test;
    // Says 1000 bytes, sends 10, then hangs up.
    const Response response = Request(test.port(), "POST /api/upload?t=" + test.token +
                                                       "&name=cut.bin HTTP/1.1\r\nContent-Length: 1000\r\n\r\n0123456789");
    CHECK(response.status == 500 || response.status == 0);
    CHECK(!fs::exists(test.folder / L"cut.bin"));
    CHECK(fs::is_empty(test.folder));
    CHECK(test.server.Transfers().at(0).state == TransferState::Failed);
    CHECK(Request(test.port(), "POST /api/upload?t=" + test.token + " HTTP/1.1\r\n\r\n").status == 411);
}

TEST(ServerSendsSharedFiles) {
    TestServer test;
    const fs::path file = TemporaryFolder() / L"shared ó.txt";
    std::ofstream(file, std::ios::binary) << "hello phone";
    const auto ids = test.server.AddFiles({file, file, TemporaryFolder() / L"missing.txt"});
    CHECK(ids.size() == 1);

    const Response state = Request(test.port(), Get("/api/state?t=" + test.token));
    CHECK(state.status == 200);
    CHECK(state.body.find("\"name\":\"shared \xC3\xB3.txt\",\"size\":11") != std::string::npos);

    const Response download = Request(test.port(), Get("/f/" + std::to_string(ids[0]) + "?t=" + test.token));
    CHECK(download.status == 200);
    CHECK(download.body == "hello phone");
    CHECK(download.head.find("filename*=UTF-8''shared%20%C3%B3.txt") != std::string::npos);
    CHECK(Request(test.port(), Get("/f/" + std::to_string(ids[0]))).status == 403);
    CHECK(Request(test.port(), Get("/f/999?t=" + test.token)).status == 404);
    CHECK(Request(test.port(), Get("/f/1x?t=" + test.token)).status == 404);

    test.server.RemoveFile(ids[0]);
    CHECK(Request(test.port(), Get("/f/" + std::to_string(ids[0]) + "?t=" + test.token)).status == 404);
    fs::remove(file);
}

TEST(ServerSharesText) {
    TestServer test;
    test.server.SetText(L"from pc ✓");
    CHECK(Request(test.port(), Get("/api/state?t=" + test.token)).body.find("\"text\":\"from pc \xE2\x9C\x93\"") !=
          std::string::npos);
    const Response sent = Request(test.port(), Post("/api/text?t=" + test.token, "https://example.com/\xC3\xB1"));
    CHECK(sent.status == 200);
    CHECK(test.server.Text() == L"https://example.com/ñ");
    CHECK(Request(test.port(), Post("/api/text?t=" + test.token, std::string(70 * 1024, 'a'))).status == 413);
}

TEST(ServerKeepsConnectionsAlive) {
    TestServer test;
    SOCKET connection = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(test.port()));
    address.sin_addr.s_addr = htonl(kLoopback);
    CHECK(connect(connection, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
    // Two pipelined requests on one connection.
    const std::string requests = "GET /api/state?t=" + test.token + " HTTP/1.1\r\n\r\n" + Post("/api/text?t=" + test.token, "hi");
    send(connection, requests.data(), static_cast<int>(requests.size()), 0);
    std::string data;
    char buffer[4096];
    int received;
    while ((received = recv(connection, buffer, sizeof(buffer), 0)) > 0) data.append(buffer, static_cast<size_t>(received));
    closesocket(connection);
    CHECK(data.find("HTTP/1.1 200") != data.rfind("HTTP/1.1 200"));
    CHECK(test.server.Text() == L"hi");
}

TEST(ServerStopsAndChangesToken) {
    TestServer test;
    const int port = test.port();
    test.server.Stop();
    CHECK(!test.server.IsRunning());
    CHECK(Request(port, Get("/?t=" + test.token)).status == 0);
    ServerOptions options;
    options.address = kLoopback;
    options.port = port;
    options.receiveFolder = test.folder;
    options.allowLoopback = true;
    CHECK(test.server.Start(options));
    CHECK(test.server.Token() != test.token);
    CHECK(Request(test.server.Port(), Get("/?t=" + test.token)).status == 403);
}

TEST(ServerRejectsNonPrivatePeers) {
    Server server;
    ServerOptions options;
    options.address = kLoopback;
    options.port = 18700;
    options.receiveFolder = TemporaryFolder();
    CHECK(server.Start(options));
    // Loopback is not a private network address: the connection is dropped.
    CHECK(Request(server.Port(), Get("/?t=" + server.Token())).status == 0);
}

TEST(ServerMovesToTheNextFreePort) {
    TestServer first;
    Server second;
    ServerOptions options;
    options.address = kLoopback;
    options.port = first.port();
    options.allowLoopback = true;
    CHECK(second.Start(options));
    CHECK(second.Port() == first.port() + 1);
}

}  // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    int failed = 0;
    for (const auto& test : Tests()) {
        try {
            test.run();
            std::cout << "PASS " << test.name << "\n";
        } catch (const Failure& failure) {
            ++failed;
            std::cout << "FAIL " << test.name << ": " << failure.message << "\n";
        } catch (const std::exception& error) {
            ++failed;
            std::cout << "FAIL " << test.name << ": " << error.what() << "\n";
        }
    }
    std::cout << (Tests().size() - failed) << "/" << Tests().size() << " tests passed\n";
    return failed ? 1 : 0;
}
