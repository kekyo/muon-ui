// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static int send_all(SOCKET client, const char *bytes, int count) {
  while (count > 0) {
    int sent = send(client, bytes, count, 0);
    if (sent <= 0) return 1;
    bytes += sent;
    count -= sent;
  }
  return 0;
}

static int serve_request(SOCKET client, const wchar_t *root) {
  // Fixture URLs are small ASCII names. Long paths are download destinations.
  char request[4096] = {0};
  size_t used = 0;
  while (strstr(request, "\r\n\r\n") == NULL) {
    if (used == sizeof(request) - 1) return 1;
    int amount = recv(client, request + used, (int)(sizeof(request) - used - 1), 0);
    if (amount <= 0) return 1;
    used += (size_t)amount;
    request[used] = '\0';
  }
  if (strncmp(request, "GET /", 5) != 0) return 1;
  char *url = request + 5;
  char *end = strchr(url, ' ');
  if (end == NULL) return 1;
  *end = '\0';
  if (strstr(url, "..") || strchr(url, '\\') || strchr(url, ':')) return 1;
  size_t base_length = wcslen(root);
  size_t length = strlen(url);
  wchar_t *path = malloc((base_length + length + 2) * sizeof(wchar_t));
  if (path == NULL) return 1;
  memcpy(path, root, base_length * sizeof(wchar_t));
  path[base_length] = L'\\';
  for (size_t i = 0; i <= length; ++i) {
    path[base_length + 1 + i] = url[i] == '/' ? L'\\' : (unsigned char)url[i];
  }
  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  free(path);
  if (file == INVALID_HANDLE_VALUE) {
    const char *missing = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    return send_all(client, missing, (int)strlen(missing));
  }
  LARGE_INTEGER size;
  if (!GetFileSizeEx(file, &size) || size.QuadPart > 1024 * 1024) {
    CloseHandle(file);
    return 1;
  }
  char header[128];
  int count = snprintf(header, sizeof(header),
      "HTTP/1.1 200 OK\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n", (unsigned long)size.QuadPart);
  int result = send_all(client, header, count);
  char buffer[4096];
  while (result == 0) {
    DWORD read = 0;
    if (!ReadFile(file, buffer, sizeof(buffer), &read, NULL)) { result = 1; break; }
    if (read == 0) break;
    result = send_all(client, buffer, (int)read);
  }
  CloseHandle(file);
  return result;
}

int wmain(int argc, wchar_t **argv) {
  if (argc != 2 || wcsncmp(argv[1], L"\\\\?\\", 4)) return 2;
  WSADATA data;
  int error = WSAStartup(MAKEWORD(2, 2), &data);
  if (error != 0) return 1;
  SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  struct sockaddr_in address = {0};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  int address_size = sizeof(address);
  if (server == INVALID_SOCKET || bind(server, (struct sockaddr *)&address, sizeof(address)) != 0 ||
      getsockname(server, (struct sockaddr *)&address, &address_size) != 0 || listen(server, 4) != 0) {
    error = WSAGetLastError();
    if (server != INVALID_SOCKET) closesocket(server);
    WSACleanup();
    fprintf(stderr, "Fixture listener failed: %d\n", error);
    return 1;
  }
  printf("READY %u\n", (unsigned)ntohs(address.sin_port));
  fflush(stdout);
  // The managed test owns this loopback-only listener and terminates it after
  // assertions. There is no firewall rule, network share or administrator setup.
  for (;;) {
    SOCKET client = accept(server, NULL, NULL);
    if (client == INVALID_SOCKET) break;
    int result = serve_request(client, argv[1]);
    closesocket(client);
    if (result != 0) break;
  }
  error = WSAGetLastError();
  closesocket(server);
  WSACleanup();
  fprintf(stderr, "Fixture request failed: %d\n", error);
  return 1;
}
