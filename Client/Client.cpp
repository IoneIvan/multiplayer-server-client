#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <map>
#include <cstring>
#include <cstdint>
#include <string>
#include <limits> // Required for std::numeric_limits
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

    // Default constructor
    SnapshotMessage() : BaseMessage(SNAPSHOT_MESSAGE, 0), snapshotData() {}

    // Parameterized constructor
    SnapshotMessage(uint8_t sender, const std::string& data)
        : BaseMessage(SNAPSHOT_MESSAGE, sender), snapshotData(data.begin(), data.end()) {}
};

class Client {
private:
    SOCKET serverSocket;
    std::thread receiveThread;
    bool isConnected;
    uint8_t clientID;

    std::vector<TextMessage> textMessages;
    std::vector<EventMessage> eventMessages;
    std::map<uint8_t, SnapshotMessage> snapshotMessages;

    std::mutex messageMutex; // Mutex for thread-safe access to message containers

public:
    Client() : isConnected(false) {}

    bool connectToServer(const std::string& serverIP);
    void disconnect();
    void sendMessage(BaseMessage* msg);
    void receiveMessages();

    // Updated Function Names
    void sortMessageByType(BaseMessage* msg);
    void processMessages();

    void displayTextMessage(TextMessage* tm);
    void processEventMessage(EventMessage* em);
    void processSnapshotMessage(SnapshotMessage* sm);
};
// Serialization and Deserialization Functions
void serializeMessage(BaseMessage* msg, std::vector<uint8_t>& buffer);
BaseMessage* deserializeMessage(const std::vector<uint8_t>& buffer);

bool Client::connectToServer(const std::string& serverIP) {
#ifdef _WIN32
    WSADATA wsData;
    WSAStartup(MAKEWORD(2, 2), &wsData);
#endif

    serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket == INVALID_SOCKET) {
        std::cerr << "Error creating socket.\n";
        return false;
    }

    sockaddr_in serverHint{};
    serverHint.sin_family = AF_INET;
    serverHint.sin_port = htons(PORT);
    inet_pton(AF_INET, serverIP.c_str(), &serverHint.sin_addr);

    if (connect(serverSocket, (sockaddr*)&serverHint, sizeof(serverHint)) == SOCKET_ERROR) {
        std::cerr << "Cannot connect to server.\n";
        return false;
    }

    isConnected = true;

    // Start receive thread
    receiveThread = std::thread(&Client::receiveMessages, this);
    receiveThread.detach();

    std::cout << "Connected to server.\n";

    return true;
}

void Client::disconnect() {
    isConnected = false;
    closesocket(serverSocket);
#ifdef _WIN32
    WSACleanup();
#endif
}

void Client::sendMessage(BaseMessage* msg) {
    std::vector<uint8_t> buffer;
    serializeMessage(msg, buffer);

    uint32_t msgSize = htonl(buffer.size());

    send(serverSocket, (char*)&msgSize, sizeof(msgSize), 0);
    send(serverSocket, (char*)buffer.data(), buffer.size(), 0);
}

void Client::receiveMessages() {
    while (isConnected) {
        uint32_t msgSize;
        int bytesReceived = recv(serverSocket, (char*)&msgSize, sizeof(msgSize), 0);
        if (bytesReceived <= 0) {
            break;
        }
        msgSize = ntohl(msgSize);

        std::vector<uint8_t> buffer(msgSize);
        size_t totalReceived = 0;
        while (totalReceived < msgSize) {
            bytesReceived = recv(serverSocket, (char*)buffer.data() + totalReceived, msgSize - totalReceived, 0);
            if (bytesReceived <= 0) {
                break;
            }
            totalReceived += bytesReceived;
        }
        if (bytesReceived <= 0) {
            break;
        }

        BaseMessage* msg = deserializeMessage(buffer);
        if (msg) {
            sortMessageByType(msg); // Call renamed function
            delete msg;
        }
    }

    disconnect();
}

void Client::sortMessageByType(BaseMessage* msg) {
    std::lock_guard<std::mutex> lock(messageMutex); // Lock for thread safety

    switch (msg->messageType) {
    case TEXT_MESSAGE: {
        TextMessage* tm = static_cast<TextMessage*>(msg);
        textMessages.push_back(*tm);
        break;
    }
    case EVENT_MESSAGE: {
        EventMessage* em = static_cast<EventMessage*>(msg);
        eventMessages.push_back(*em);
        break;
    }
    case SNAPSHOT_MESSAGE: {
        SnapshotMessage* sm = static_cast<SnapshotMessage*>(msg);
        snapshotMessages[sm->senderID] = *sm;
        break;
    }
    }
}
void Client::processMessages() {
    while (isConnected) {
        {
            std::lock_guard<std::mutex> lock(messageMutex); // Lock for thread safety

            // Process and remove each TextMessage
            for (auto it = textMessages.begin(); it != textMessages.end(); ) {
                displayTextMessage(&(*it));
                it = textMessages.erase(it); // Erase and advance the iterator
            }

            // Process and remove each EventMessage
            for (auto it = eventMessages.begin(); it != eventMessages.end(); ) {
                processEventMessage(&(*it));
                it = eventMessages.erase(it); // Erase and advance the iterator
            }

            // Process and remove each SnapshotMessage
            for (auto it = snapshotMessages.begin(); it != snapshotMessages.end(); ) {
                processSnapshotMessage(&it->second);
                it = snapshotMessages.erase(it); // Erase and advance the iterator
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Prevent tight loop
    }
}

void Client::displayTextMessage(TextMessage* tm) {
    std::cout << "Received text message from Client " << (int)tm->senderID << ": ";
    for (auto ch : tm->text) {
        std::cout << (char)ch;
    }
    std::cout << std::endl;
}

void Client::processEventMessage(EventMessage* em) {
    std::cout << "Processing event message from Client " << (int)em->senderID << std::endl;
}

void Client::processSnapshotMessage(SnapshotMessage* sm) {
    std::cout << "Received snapshot from Client " << (int)sm->senderID << std::endl;
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
    Client client;
    std::string serverIP;

    std::cout << "Enter server IP address: ";
    std::cin >> serverIP;

    if (!client.connectToServer(serverIP)) {
        std::cerr << "Failed to connect to server.\n";
        return 1;
    }

    std::cin.ignore(); // Clear newline character from input buffer

    // Start a thread to process messages
    std::thread processingThread(&Client::processMessages, &client);

    while (true) {
        std::cout << "Enter message type (0: Text, 1: Event, 2: Snapshot, 9: Exit): ";
        int msgType;
        std::cin >> msgType;
        std::cin.ignore();
        std::cin.clear();

        if (msgType == 9) {
            break;
        }

        std::cout << "Enter message content: ";
        std::string content;
        std::getline(std::cin, content);

        BaseMessage* msg = nullptr;

        switch (msgType) {
        case TEXT_MESSAGE:
            msg = new TextMessage(0, content);
            client.sendMessage(msg);
            delete msg;
            break;
        case EVENT_MESSAGE:
            msg = new EventMessage(0, content);
            client.sendMessage(msg);
            delete msg;
            break;
        case SNAPSHOT_MESSAGE:
            for (int i = 0; i < 1999999; i++) {
                msg = new SnapshotMessage(0, content);
                client.sendMessage(msg);
                delete msg;
            }
            break;
        default:
            std::cout << "Invalid message type.\n";
            continue;
        }
    }

    client.disconnect();
    processingThread.join(); // Wait for processing thread to finish

    return 0;
}
