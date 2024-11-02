#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <map>
#include <cstring>
#include <cstdint>

// Platform-specific includes
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h> // Include this header for InetPton
#pragma comment(lib, "ws2_32.lib")
typedef int socklen_t;
#else
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#endif

#define PORT 54000

// Message Type Constants
const uint8_t TEXT_MESSAGE = 0;
const uint8_t EVENT_MESSAGE = 1;
const uint8_t SNAPSHOT_MESSAGE = 2;

// Message Base Class
class BaseMessage {
public:
    uint8_t messageType;
    uint8_t senderID;

    BaseMessage(uint8_t type, uint8_t sender)
        : messageType(type), senderID(sender) {}

    virtual ~BaseMessage() {}
};

// Derived Message Classes
class TextMessage : public BaseMessage {
public:
    std::vector<uint8_t> text;

    TextMessage(uint8_t sender, const std::string& msg)
        : BaseMessage(TEXT_MESSAGE, sender), text(msg.begin(), msg.end()) {}
};

class EventMessage : public BaseMessage {
public:
    std::vector<uint8_t> eventData;

    EventMessage(uint8_t sender, const std::string& data)
        : BaseMessage(EVENT_MESSAGE, sender), eventData(data.begin(), data.end()) {}
};

class SnapshotMessage : public BaseMessage {
public:
    std::vector<uint8_t> snapshotData;

    SnapshotMessage(uint8_t sender, const std::string& data)
        : BaseMessage(SNAPSHOT_MESSAGE, sender), snapshotData(data.begin(), data.end()) {}
};

// Client Handler Struct
struct ClientHandler {
    SOCKET socket;
    uint8_t clientID;
    std::thread thread;
};

class Server {
private:
    SOCKET listeningSocket;
    std::vector<ClientHandler*> clients;
    uint8_t nextClientID;
    std::mutex clientsMutex;
    bool isRunning;

public:
    Server() : nextClientID(1), isRunning(true) {}

    void start();
    void acceptClients();
    void handleClient(ClientHandler* clientHandler);
    void broadcastMessage(BaseMessage* msg, uint8_t excludeID = 0);
    void stop();
};

// Serialization and Deserialization Functions
void serializeMessage(BaseMessage* msg, std::vector<uint8_t>& buffer);
BaseMessage* deserializeMessage(const std::vector<uint8_t>& buffer);

void Server::start() {
    // Initialize platform-specific networking
#ifdef _WIN32
    WSADATA wsData;
    WSAStartup(MAKEWORD(2, 2), &wsData);
#endif

    // Create listening socket
    listeningSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (listeningSocket == INVALID_SOCKET) {
        std::cerr << "Error creating socket.\n";
        return;
    }

    // Bind socket to IP and port
    sockaddr_in serverHint{};
    serverHint.sin_family = AF_INET;
    serverHint.sin_port = htons(PORT);
    serverHint.sin_addr.s_addr = INADDR_ANY;

    if (bind(listeningSocket, (sockaddr*)&serverHint, sizeof(serverHint)) == SOCKET_ERROR) {
        std::cerr << "Error binding socket.\n";
        return;
    }

    // Start listening
    listen(listeningSocket, SOMAXCONN);

    std::cout << "Server is listening on port " << PORT << "...\n";

    // Accept clients in a separate thread
    std::thread(&Server::acceptClients, this).detach();
}

void Server::acceptClients() {
    while (isRunning) {
        sockaddr_in clientHint{};
        socklen_t clientSize = sizeof(clientHint);

        SOCKET clientSocket = accept(listeningSocket, (sockaddr*)&clientHint, &clientSize);
        if (clientSocket != INVALID_SOCKET) {
            // Assign a unique ID to the new client
            uint8_t clientID = nextClientID++;

            // Create a new client handler
            ClientHandler* clientHandler = new ClientHandler{ clientSocket, clientID };
            {
                std::lock_guard<std::mutex> lock(clientsMutex);
                clients.push_back(clientHandler);
            }

            // Notify existing clients about the new client
            // ...

            // Start client thread
            clientHandler->thread = std::thread(&Server::handleClient, this, clientHandler);
            clientHandler->thread.detach();

            std::cout << "Client " << (int)clientID << " connected.\n";
        }
    }
}

void Server::handleClient(ClientHandler* clientHandler) {
    SOCKET clientSocket = clientHandler->socket;
    uint8_t clientID = clientHandler->clientID;

    while (isRunning) {
        // Receive message size
        uint32_t msgSize;
        int bytesReceived = recv(clientSocket, (char*)&msgSize, sizeof(msgSize), 0);
        if (bytesReceived <= 0) {
            break;
        }
        msgSize = ntohl(msgSize);

        // Receive message data
        std::vector<uint8_t> buffer(msgSize);
        size_t totalReceived = 0;
        while (totalReceived < msgSize) {
            bytesReceived = recv(clientSocket, (char*)buffer.data() + totalReceived, msgSize - totalReceived, 0);
            if (bytesReceived <= 0) {
                break;
            }
            totalReceived += bytesReceived;
        }
        if (bytesReceived <= 0) {
            break;
        }

        // Deserialize message
        BaseMessage* msg = deserializeMessage(buffer);
        if (msg) {
            msg->senderID = clientID;
            // Broadcast the message to other clients
            broadcastMessage(msg, clientID);
            delete msg;
        }
    }

    // Remove client from list
    {
        std::lock_guard<std::mutex> lock(clientsMutex);
        clients.erase(std::remove_if(clients.begin(), clients.end(),
            [clientID](ClientHandler* ch) { return ch->clientID == clientID; }), clients.end());
    }

    // Notify other clients about client disconnect
    // ...

    closesocket(clientSocket);
    std::cout << "Client " << (int)clientID << " disconnected.\n";
}

void Server::broadcastMessage(BaseMessage* msg, uint8_t excludeID) {
    std::vector<uint8_t> buffer;
    serializeMessage(msg, buffer);

    uint32_t msgSize = htonl(buffer.size());

    std::lock_guard<std::mutex> lock(clientsMutex);
    for (ClientHandler* clientHandler : clients) {
        if (clientHandler->clientID != excludeID) {
            send(clientHandler->socket, (char*)&msgSize, sizeof(msgSize), 0);
            send(clientHandler->socket, (char*)buffer.data(), buffer.size(), 0);
        }
    }
}

void Server::stop() {
    isRunning = false;
    closesocket(listeningSocket);
#ifdef _WIN32
    WSACleanup();
#endif
}

// Serialization Function
void serializeMessage(BaseMessage* msg, std::vector<uint8_t>& buffer) {
    buffer.push_back(msg->messageType);
    buffer.push_back(msg->senderID);

    switch (msg->messageType) {
    case TEXT_MESSAGE: {
        TextMessage* tm = static_cast<TextMessage*>(msg);
        uint32_t length = htonl(tm->text.size());
        buffer.insert(buffer.end(), (uint8_t*)&length, (uint8_t*)&length + sizeof(length));
        buffer.insert(buffer.end(), tm->text.begin(), tm->text.end());
        break;
    }
    case EVENT_MESSAGE: {
        EventMessage* em = static_cast<EventMessage*>(msg);
        uint32_t length = htonl(em->eventData.size());
        buffer.insert(buffer.end(), (uint8_t*)&length, (uint8_t*)&length + sizeof(length));
        buffer.insert(buffer.end(), em->eventData.begin(), em->eventData.end());
        break;
    }
    case SNAPSHOT_MESSAGE: {
        SnapshotMessage* sm = static_cast<SnapshotMessage*>(msg);
        uint32_t length = htonl(sm->snapshotData.size());
        buffer.insert(buffer.end(), (uint8_t*)&length, (uint8_t*)&length + sizeof(length));
        buffer.insert(buffer.end(), sm->snapshotData.begin(), sm->snapshotData.end());
        break;
    }
    }
}

// Deserialization Function
BaseMessage* deserializeMessage(const std::vector<uint8_t>& buffer) {
    if (buffer.size() < 2) return nullptr;

    uint8_t messageType = buffer[0];
    uint8_t senderID = buffer[1];
    size_t offset = 2;

    switch (messageType) {
    case TEXT_MESSAGE: {
        if (buffer.size() < offset + 4) return nullptr;
        uint32_t length;
        memcpy(&length, &buffer[offset], 4);
        length = ntohl(length);
        offset += 4;

        if (buffer.size() < offset + length) return nullptr;

        std::string text(buffer.begin() + offset, buffer.begin() + offset + length);
        return new TextMessage(senderID, text);
    }
    case EVENT_MESSAGE: {
        if (buffer.size() < offset + 4) return nullptr;
        uint32_t length;
        memcpy(&length, &buffer[offset], 4);
        length = ntohl(length);
        offset += 4;

        if (buffer.size() < offset + length) return nullptr;

        std::string data(buffer.begin() + offset, buffer.begin() + offset + length);
        return new EventMessage(senderID, data);
    }
    case SNAPSHOT_MESSAGE: {
        if (buffer.size() < offset + 4) return nullptr;
        uint32_t length;
        memcpy(&length, &buffer[offset], 4);
        length = ntohl(length);
        offset += 4;

        if (buffer.size() < offset + length) return nullptr;

        std::string data(buffer.begin() + offset, buffer.begin() + offset + length);
        return new SnapshotMessage(senderID, data);
    }
    default:
        return nullptr;
    }
}

int main() {
    Server server;
    server.start();

    std::cout << "Press Enter to stop the server...\n";
    std::cin.get();

    server.stop();

    return 0;
}
