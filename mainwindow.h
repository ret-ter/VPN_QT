#ifndef MAINWINDOW_H
#define MAINWINDOW_H


#include <QWidget>
#include <QPushButton>
#include <QTextEdit>
#include <QLineEdit>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <string>
#include <thread>

class ClientThread : public QThread {
    Q_OBJECT

public:
    explicit ClientThread(QObject* parent = nullptr);
    ~ClientThread();

    void disconnect();
    void sendMessage(const QString& message);

    enum class ConnectionErrorType {
        SocketCreationFailed,
        ConnectionFailed,
        SSLConnectionFailed,
        UnknownError
    };

signals:
    void messageReceived(const QString& message);
    void disconnected();
    void messageSent(const QString& message);
    void serverGoneDown();
    void connectionFailed(ConnectionErrorType errorType, const QString& errorMessage);
    void connectionSuccessful();

protected:
    void run() override;

private:
    void initialize_openssl();
    void cleanup_openssl();
    SSL_CTX* create_context();
    void configure_context(SSL_CTX* ctx);
    void log_certificate(SSL* ssl);
    void read_from_server();

    SSL_CTX* ctx;
    SSL* ssl;
    int sockfd;
    bool running;
    bool disconnectRequested;
    std::thread readerThread;
    QMutex mutex;
    QWaitCondition condition;
    QString messageToSend;
};




class ClientWindow : public QWidget {
    Q_OBJECT

public:
    explicit ClientWindow(QWidget* parent = nullptr);
    ~ClientWindow();

private slots:
    void onConnectClicked();
    void onDisconnectClicked();
    void onSendClicked();
    void onMessageReceived(const QString& message);
    void onMessageSent(const QString& message);  // Slot to handle sent messages
    void onDisconnected();
    void onServerGoneDown();  // New slot
    void onConnectionFailed(ClientThread::ConnectionErrorType errorType, const QString& errorMessage);  // New slot
    void onConnectionSuccessful();

private:
    QPushButton* connectButton;
    QPushButton* disconnectButton;
    QPushButton* sendButton;
    QTextEdit* messageArea;
    QLineEdit* inputField;
    ClientThread* clientThread;
};




#endif // MAINWINDOW_H
