#include "mainwindow.h"

#include <QMessageBox>
#include <QMetaObject>
#include <QDebug>
#include <cstring>      // For memset
#include <unistd.h>     // For close
#include <arpa/inet.h>  // For inet_pton
#include <sys/socket.h> // For socket, connect, etc.
#include <netinet/in.h> // For sockaddr_in
#include <openssl/ssl.h>
#include <openssl/err.h>

#define PORT 4443
#define SERVER_IP "127.0.0.1"

ClientThread::ClientThread(QObject* parent)
    : QThread(parent), ctx(nullptr), ssl(nullptr), sockfd(-1), running(false), disconnectRequested(false) {
}

ClientThread::~ClientThread() {
    disconnect();
    if (readerThread.joinable()) {
        readerThread.join();
    }
}

void ClientThread::initialize_openssl() {
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();
}

void ClientThread::cleanup_openssl() {
    EVP_cleanup();
}

SSL_CTX* ClientThread::create_context() {
    const SSL_METHOD* method = TLS_client_method();
    SSL_CTX* ctx = SSL_CTX_new(method);

    if (!ctx) {
        perror("Unable to create SSL context");
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }
    return ctx;
}

void ClientThread::configure_context(SSL_CTX* ctx) {
    try {
        if (SSL_CTX_use_certificate_file(ctx, "/Users/konst/myCA/certs/client-cert.pem", SSL_FILETYPE_PEM) <= 0) {
            throw std::runtime_error("Failed to load client certificate");
        }

        if (SSL_CTX_use_PrivateKey_file(ctx, "/Users/konst/myCA/private/client-key-no-pass.pem", SSL_FILETYPE_PEM) <= 0) {
            throw std::runtime_error("Failed to load private key");
        }

        if (!SSL_CTX_load_verify_locations(ctx, "/Users/konst/myCA/certs/ca-cert.pem", nullptr)) {
            throw std::runtime_error("Failed to load CA certificate");
        }

        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        SSL_CTX_set_verify_depth(ctx, 1);
    } catch (const std::runtime_error& e) {
        qCritical() << "Exception in configure_context: " << e.what();
        emit connectionFailed(ConnectionErrorType::SSLConnectionFailed, e.what());
        throw;  // Re-throw the exception to ensure the caller is aware of the failure
    }
}



void ClientThread::log_certificate(SSL* ssl) {
    X509* cert = SSL_get_peer_certificate(ssl);
    if (cert) {
        char* subj = X509_NAME_oneline(X509_get_subject_name(cert), 0, 0);
        char* issuer = X509_NAME_oneline(X509_get_issuer_name(cert), 0, 0);
        if (subj && issuer) {
            qDebug() << "Client certificate subject:" << subj;
            qDebug() << "Client certificate issuer:" << issuer;
        } else {
            qDebug() << "Unable to retrieve certificate details.";
        }
        OPENSSL_free(subj);
        OPENSSL_free(issuer);
        X509_free(cert);
    } else {
        qDebug() << "No client certificate presented.";
    }
}

void ClientThread::read_from_server() {
    char buffer[256];
    while (running) {
        int bytes = SSL_read(ssl, buffer, sizeof(buffer));
        if (bytes <= 0) {
            ERR_print_errors_fp(stderr);
            running = false;
            emit serverGoneDown();  // Emit the signal when server goes down
            emit disconnected();
            break;
        }
        buffer[bytes] = '\0';
        emit messageReceived(QString(buffer));
    }
}


void ClientThread::run() {
    try {
        initialize_openssl();
        ctx = create_context();
        configure_context(ctx);

        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            perror("Unable to create socket");
            cleanup_openssl();
            emit connectionFailed(ConnectionErrorType::SocketCreationFailed, "Unable to create socket");
            return;
        }

        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(PORT);
        inet_pton(AF_INET, SERVER_IP, &addr.sin_addr);

        if (::connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            perror("Unable to connect");
            close(sockfd);
            SSL_CTX_free(ctx);
            cleanup_openssl();
            emit connectionFailed(ConnectionErrorType::ConnectionFailed, "Socket Creation Failed");
            return;
        }

        ssl = SSL_new(ctx);
        SSL_set_fd(ssl, sockfd);

        if (SSL_connect(ssl) <= 0) {
            ERR_print_errors_fp(stderr);
            emit connectionFailed(ConnectionErrorType::SSLConnectionFailed, "SSL connection failed");
            return;
        }
        emit connectionSuccessful();

        running = true;
        disconnectRequested = false;  // Reset the disconnect request flag
        log_certificate(ssl);

        readerThread = std::thread(&ClientThread::read_from_server, this);

        while (running) {
            QMutexLocker locker(&mutex);
            condition.wait(&mutex);
            if (disconnectRequested) {
                running = false;
                break;
            }
            if (!messageToSend.isEmpty()) {
                SSL_write(ssl, messageToSend.toUtf8().constData(), messageToSend.length());
                emit messageSent(messageToSend);
                messageToSend.clear();
            }
        }

        if (readerThread.joinable()) {
            readerThread.join();
        }


        if (ssl) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
            ssl = nullptr;
        }
        if (sockfd != -1) {
            close(sockfd);
            sockfd = -1;
        }
        if (ctx) {
            SSL_CTX_free(ctx);
            ctx = nullptr;
        }
        cleanup_openssl();

        emit disconnected();
    } catch (const std::exception& e) {
        qCritical() << "Exception in ClientThread: " << e.what();
        emit connectionFailed(ConnectionErrorType::UnknownError, e.what());


        if (ssl) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
            ssl = nullptr;
        }
        if (sockfd != -1) {
            close(sockfd);
            sockfd = -1;
        }
        if (ctx) {
            SSL_CTX_free(ctx);
            ctx = nullptr;
        }
        cleanup_openssl();
    }
}


void ClientThread::disconnect() {
    {
        QMutexLocker locker(&mutex);
        if (running) {
            disconnectRequested = true;
            condition.wakeAll();
        }
    }

    // Immediate cleanup of SSL and socket

    if (sockfd != -1) {
        close(sockfd);
        sockfd = -1;
    }

    cleanup_openssl();

    emit disconnected();  // Emit the signal immediately after cleanup
}

void ClientThread::sendMessage(const QString& message) {
    QMutexLocker locker(&mutex);
    messageToSend = message;
    condition.wakeAll();
}

ClientWindow::ClientWindow(QWidget* parent)
    : QWidget(parent), clientThread(new ClientThread(this)) {
    connectButton = new QPushButton("Connect", this);
    disconnectButton = new QPushButton("Disconnect", this);
    sendButton = new QPushButton("Send", this);
    messageArea = new QTextEdit(this);
    inputField = new QLineEdit(this);

    QVBoxLayout* layout = new QVBoxLayout(this);
    QHBoxLayout* buttonLayout = new QHBoxLayout();
    QHBoxLayout* inputLayout = new QHBoxLayout();

    buttonLayout->addWidget(connectButton);
    buttonLayout->addWidget(disconnectButton);

    inputLayout->addWidget(inputField);
    inputLayout->addWidget(sendButton);

    layout->addLayout(buttonLayout);
    layout->addWidget(messageArea);
    layout->addLayout(inputLayout);

    setLayout(layout);

    // Set size policy, minimum size, and maximum size for messageArea
    messageArea->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    messageArea->setMaximumSize(400, 300);  // Set the maximum size as per your requirement

    messageArea->setReadOnly(true);
    disconnectButton->setEnabled(false);
    sendButton->setEnabled(false);
    // Set stylesheet for background image and colors
    // Set stylesheet for background image and colors
    setStyleSheet("QWidget {"
                  "background: qlineargradient(spread:pad, x1:0, y1:0, x2:1, y2:1, "
                  "stop:0 #4cc9f0, stop:1 #3a0ca3);"  // Gradient from #FF5A5F to a darker red
                  "}"
                  "QPushButton {"
                  "background-color: black;"
                  "color: #edf2f4;"
                  "border: 1px solid #001d3d;"
                  "border-radius: 15px;"
                  "padding: 10px 20px;"
                  "font-size: 18px;"
                  "}"
                  "QPushButton:hover {"
                  "background-color: #001d3d;"
                  "color: white;"
                  "}"
                  "QTextEdit {"
                  "background-color: rgba(0, 0, 0, 0.8);"  // Semi-transparent dark background
                  "color: white;"  // Text color
                  "border: 1px solid #ccc;"
                  "padding: 10px;"
                  "font-size: 16px;"
                  "}"
                  "QLineEdit {"
                  "background-color: rgba(0, 0, 0, 0.8);"  // Semi-transparent dark background
                  "color: white;"  // Text color
                  "border: 1px solid #ccc;"
                  "padding: 10px;"
                  "font-size: 16px;"
                  "}");


    connect(connectButton, &QPushButton::clicked, this, &ClientWindow::onConnectClicked);
    connect(disconnectButton, &QPushButton::clicked, this, &ClientWindow::onDisconnectClicked);
    connect(sendButton, &QPushButton::clicked, this, &ClientWindow::onSendClicked);
    connect(clientThread, &ClientThread::messageReceived, this, &ClientWindow::onMessageReceived);
    connect(clientThread, &ClientThread::disconnected, this, &ClientWindow::onDisconnected);
    connect(clientThread, &ClientThread::messageSent, this, &ClientWindow::onMessageSent);
    connect(clientThread, &ClientThread::serverGoneDown, this, &ClientWindow::onServerGoneDown);
    connect(clientThread, &ClientThread::connectionFailed, this, &ClientWindow::onConnectionFailed);
    connect(clientThread, &ClientThread::connectionSuccessful, this, &ClientWindow::onConnectionSuccessful);





    // Default window size
    resize(414, 700);
}

ClientWindow::~ClientWindow() {
    if (clientThread->isRunning()) {
        clientThread->disconnect();
        clientThread->wait();
    }
}
void ClientWindow::onConnectionFailed(ClientThread::ConnectionErrorType errorType, const QString& errorMessage) {
    QString fullMessage;
    switch (errorType) {
    case ClientThread::ConnectionErrorType::SocketCreationFailed:
        fullMessage = "Socket Creation Failed: " + errorMessage;
        break;
    case ClientThread::ConnectionErrorType::ConnectionFailed:
        fullMessage = "Connection Failed: " + errorMessage;
        break;
    case ClientThread::ConnectionErrorType::SSLConnectionFailed:
        fullMessage = "SSL Connection Failed: " + errorMessage;
        break;
    default:
        fullMessage = "Unknown Error: " + errorMessage;
        break;
    }

    QMessageBox errorMsgBox;
    errorMsgBox.setWindowTitle("Connection Error");
    errorMsgBox.setText(fullMessage);

    // Customize the stylesheet for error text and background
    errorMsgBox.setStyleSheet("QMessageBox {"
                              "background-color: #ffcccb;"  // Light red background
                              "}"
                              "QLabel {"
                              "color: #b22222;"  // Dark red text
                              "font-size: 14px;"
                              "}"
                              "QPushButton {"
                              "background-color: #b22222;"  // Dark red button background
                              "color: white;"  // White text on button
                              "border-radius: 5px;"
                              "padding: 5px 10px;"
                              "}");

    errorMsgBox.exec();

    connectButton->setEnabled(true);
    disconnectButton->setEnabled(false);
    sendButton->setEnabled(false);
}



void ClientWindow::onConnectionSuccessful() {
    messageArea->append("Successfully connected to the server.");
}

void ClientWindow::onServerGoneDown() {
    messageArea->append("Server has gone down. Please try again later.");
    connectButton->setEnabled(true);
    disconnectButton->setEnabled(false);
    sendButton->setEnabled(false);
}

void ClientWindow::onConnectClicked() {
    connectButton->setEnabled(false);
    disconnectButton->setEnabled(true);
    sendButton->setEnabled(true);
    clientThread->start();
}

void ClientWindow::onDisconnectClicked() {
    clientThread->disconnect();
    messageArea->append("Disconnected from server.");
}

void ClientWindow::onSendClicked() {
    QString message = inputField->text();
    if (!message.isEmpty()) {
        clientThread->sendMessage(message);
        inputField->clear();
    }
}

void ClientWindow::onMessageReceived(const QString& message) {
    messageArea->append("Received: " + message);
}

void ClientWindow::onMessageSent(const QString& message) {
    messageArea->append("Sent: " + message);
}

void ClientWindow::onDisconnected() {
    connectButton->setEnabled(true);
    disconnectButton->setEnabled(false);
    sendButton->setEnabled(false);

}
