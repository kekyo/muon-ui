// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <stdio.h>
#include <stdlib.h>

typedef struct RelaySockets {
  SOCKET left;
  SOCKET right;
} RelaySockets;

static int forward_bytes(SOCKET source, SOCKET destination) {
  char buffer[32768];
  const int received = recv(source, buffer, sizeof(buffer), 0);
  if (received <= 0) {
    return 0;
  }
  int sent_total = 0;
  while (sent_total < received) {
    const int sent =
        send(destination, buffer + sent_total, received - sent_total, 0);
    if (sent <= 0) {
      return 0;
    }
    sent_total += sent;
  }
  return 1;
}

static DWORD WINAPI relay_connection(LPVOID parameter) {
  RelaySockets* sockets = (RelaySockets*)parameter;
  for (;;) {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(sockets->left, &read_set);
    FD_SET(sockets->right, &read_set);
    const int ready = select(0, &read_set, NULL, NULL, NULL);
    if (ready <= 0) {
      break;
    }
    if (FD_ISSET(sockets->left, &read_set) &&
        !forward_bytes(sockets->left, sockets->right)) {
      break;
    }
    if (FD_ISSET(sockets->right, &read_set) &&
        !forward_bytes(sockets->right, sockets->left)) {
      break;
    }
  }
  shutdown(sockets->left, SD_BOTH);
  shutdown(sockets->right, SD_BOTH);
  closesocket(sockets->left);
  closesocket(sockets->right);
  free(sockets);
  return 0;
}

static int parse_port(const char* value, const char* label) {
  char* end = NULL;
  const long port = strtol(value, &end, 10);
  if (end == value || *end != '\0' || port < 1 || port > 65535) {
    fprintf(stderr, "Invalid %s: %s\n", label, value);
    return -1;
  }
  return (int)port;
}

static SOCKET connect_target(int target_port) {
  SOCKET target = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (target == INVALID_SOCKET) {
    return INVALID_SOCKET;
  }

  struct sockaddr_in address;
  address.sin_family = AF_INET;
  address.sin_port = htons((u_short)target_port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(target, (const struct sockaddr*)&address, sizeof(address)) != 0) {
    closesocket(target);
    return INVALID_SOCKET;
  }
  return target;
}

static void start_relay_connection(SOCKET left, SOCKET right) {
  RelaySockets* sockets = (RelaySockets*)malloc(sizeof(RelaySockets));
  if (sockets == NULL) {
    closesocket(left);
    closesocket(right);
    return;
  }
  sockets->left = left;
  sockets->right = right;
  const HANDLE thread =
      CreateThread(NULL, 0, relay_connection, sockets, 0, NULL);
  if (thread != NULL) {
    CloseHandle(thread);
  }
}

int main(int argc, char** argv) {
  if (argc != 3) {
    fprintf(stderr, "Usage: %s <listen-port> <target-port>\n", argv[0]);
    return 2;
  }

  const int listen_port = parse_port(argv[1], "listen port");
  const int target_port = parse_port(argv[2], "target port");
  if (listen_port < 0 || target_port < 0) {
    return 2;
  }

  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    fprintf(stderr, "WSAStartup failed\n");
    return 1;
  }

  SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET) {
    fprintf(stderr, "socket failed: %d\n", WSAGetLastError());
    WSACleanup();
    return 1;
  }

  BOOL reuse_address = TRUE;
  setsockopt(
      listener,
      SOL_SOCKET,
      SO_REUSEADDR,
      (const char*)&reuse_address,
      sizeof(reuse_address));

  struct sockaddr_in address;
  address.sin_family = AF_INET;
  address.sin_port = htons((u_short)listen_port);
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(listener, (const struct sockaddr*)&address, sizeof(address)) != 0) {
    fprintf(stderr, "bind failed: %d\n", WSAGetLastError());
    closesocket(listener);
    WSACleanup();
    return 1;
  }
  if (listen(listener, SOMAXCONN) != 0) {
    fprintf(stderr, "listen failed: %d\n", WSAGetLastError());
    closesocket(listener);
    WSACleanup();
    return 1;
  }

  for (;;) {
    SOCKET client = accept(listener, NULL, NULL);
    if (client == INVALID_SOCKET) {
      continue;
    }
    SOCKET target = connect_target(target_port);
    if (target == INVALID_SOCKET) {
      closesocket(client);
      continue;
    }
    start_relay_connection(client, target);
  }
}
