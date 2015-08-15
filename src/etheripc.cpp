    /*
    This file is part of etherwall.
    etherwall is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    etherwall is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    You should have received a copy of the GNU General Public License
    along with etherwall. If not, see <http://www.gnu.org/licenses/>.
*/
/** @file etheripc.cpp
 * @author Ales Katona <almindor@gmail.com>
 * @date 2015
 *
 * Ethereum IPC client implementation
 */

#include "etheripc.h"
#include "bigint.h"
#include <QJsonDocument>
#include <QJsonValue>
#include <QTimer>

namespace Etherwall {

// *************************** RequestIPC **************************** //
    int RequestIPC::sCallID = 0;

    RequestIPC::RequestIPC(RequestTypes type, const QString method, const QJsonArray params, int index) :
        fCallID(sCallID++), fType(type), fMethod(method), fParams(params), fIndex(index), fEmpty(false)
    {
    }

    RequestIPC::RequestIPC(bool empty) : fEmpty(empty)
    {
    }

    RequestIPC::RequestIPC() : fEmpty(true)
    {
    }

    RequestTypes RequestIPC::getType() const {
        return fType;
    }

    const QString& RequestIPC::getMethod() const {
        return fMethod;
    }

    const QJsonArray& RequestIPC::getParams() const {
        return fParams;
    }

    int RequestIPC::getIndex() const {
        return fIndex;
    }

    int RequestIPC::getCallID() const {
        return fCallID;
    }

    bool RequestIPC::empty() const {
        return fEmpty;
    }

// *************************** EtherIPC **************************** //

    EtherIPC::EtherIPC()
    {
        connect(&fSocket, (void (QLocalSocket::*)(QLocalSocket::LocalSocketError))&QLocalSocket::error, this, &EtherIPC::onSocketError);
        connect(&fSocket, &QLocalSocket::readyRead, this, &EtherIPC::onSocketReadyRead);
        connect(&fSocket, &QLocalSocket::connected, this, &EtherIPC::connectedToServer);
        connect(&fSocket, &QLocalSocket::disconnected, this, &EtherIPC::disconnectedFromServer);
    }

    bool EtherIPC::getBusy() const {
        return !fActiveRequest.empty();
    }

    const QString& EtherIPC::getError() const {
        return fError;
    }

    int EtherIPC::getCode() const {
        return fCode;
    }

    void EtherIPC::closeApp() {
        fSocket.abort();
        thread()->quit();
    }

    void EtherIPC::connectToServer(const QString& path) {
        fActiveRequest = RequestIPC(false);
        emit busyChanged(getBusy());
        fPath = path;
        if ( fSocket.state() != QLocalSocket::UnconnectedState ) {
            fError = "Already connected";
            return bail();
        }

        fSocket.connectToServer(path);
        QTimer::singleShot(2000, this, SLOT(disconnectedFromServer()));
    }

    void EtherIPC::connectedToServer() {
        done();

        newPendingTransactionFilter();

        emit connectToServerDone();
        emit connectionStateChanged();
    }

    void EtherIPC::disconnectedFromServer() {
        if ( fSocket.state() == QLocalSocket::UnconnectedState ) { // could be just the bloody timer
            fError = fSocket.errorString();
            bail();
        }
    }

    void EtherIPC::getAccounts() {
        if ( !queueRequest(RequestIPC(GetAccountRefs, "personal_listAccounts", QJsonArray())) ) {
            return bail();
        }
    }

    void EtherIPC::handleAccountDetails() {
        QJsonValue jv;
        if ( !readReply(jv) ) {
            return bail();
        }

        QJsonArray refs = jv.toArray();
        int i = 0;
        foreach( QJsonValue r, refs ) {
            const QString hash = r.toString("INVALID");
            fAccountList.append(AccountInfo(hash, QString(), -1));
            QJsonArray params;
            params.append(hash);
            params.append("latest");
            if ( !queueRequest(RequestIPC(GetBalance, "eth_getBalance", params, i)) ) {
                return bail();
            }

            if ( !queueRequest(RequestIPC(GetTransactionCount, "eth_getTransactionCount", params, i++)) ) {
                return bail();
            }
        }

        done();
    }

    void EtherIPC::handleAccountBalance() {
        QJsonValue jv;
        if ( !readReply(jv) ) {
            return bail();
        }

        const QString decStr = toDecStr(jv);
        fAccountList[fActiveRequest.getIndex()].setBalance(decStr);

        done();
    }

    void EtherIPC::handleAccountTransactionCount() {
        QJsonValue jv;
        if ( !readReply(jv) ) {
            return bail();
        }

        std::string hexStr = jv.toString("0x0").remove(0, 2).toStdString();
        const BigInt::Vin bv(hexStr, 16);
        quint64 count = bv.toUlong();
        fAccountList[fActiveRequest.getIndex()].setTransactionCount(count);

        if ( fActiveRequest.getIndex() + 1 == fAccountList.length() ) {
            emit getAccountsDone(fAccountList);
        }
        done();
    }

    void EtherIPC::newAccount(const QString& password, int index) {
        QJsonArray params;
        params.append(password);
        if ( !queueRequest(RequestIPC(NewAccount, "personal_newAccount", params, index)) ) {
            return bail();
        }
    }

    void EtherIPC::handleNewAccount() {
        QJsonValue jv;
        if ( !readReply(jv) ) {
            return bail();
        }

        const QString result = jv.toString();
        emit newAccountDone(result, fActiveRequest.getIndex());
        done();
    }

    void EtherIPC::deleteAccount(const QString& hash, const QString& password, int index) {
        QJsonArray params;
        params.append(hash);
        params.append(password);        
        if ( !queueRequest(RequestIPC(DeleteAccount, "personal_deleteAccount", params, index)) ) {
            return bail();
        }
    }

    void EtherIPC::handleDeleteAccount() {
        QJsonValue jv;
        if ( !readReply(jv) ) {
            return bail();
        }

        const bool result = jv.toBool(false);
        emit deleteAccountDone(result, fActiveRequest.getIndex());
        done();
    }

    void EtherIPC::getBlockNumber() {
        if ( !queueRequest(RequestIPC(GetBlockNumber, "eth_blockNumber")) ) {
            return bail();
        }
    }

    void EtherIPC::handleGetBlockNumber() {
        quint64 result;
        if ( !readNumber(result) ) {
             return bail();
        }

        emit getBlockNumberDone(result);
        done();
    }

    void EtherIPC::getPeerCount() {
        if ( !queueRequest(RequestIPC(GetPeerCount, "net_peerCount")) ) {
            return bail();
        }
    }

    void EtherIPC::handleGetPeerCount() {
        if ( !readNumber(fPeerCount) ) {
             return bail();
        }

        emit peerCountChanged(fPeerCount);
        done();
    }

    void EtherIPC::sendTransaction(const QString& from, const QString& to, double value) {
        if ( value <= 0 ) {
            fError = "Invalid transaction value";
            return bail();
        }

        QJsonArray params;
        BigInt::Vin vinVal = BigInt::Vin::fromDouble(value * 1000000000000000000);
        QString strHex = QString(vinVal.toStr0xHex().data());

        QJsonObject p;
        p["from"] = from;
        p["to"] = to;
        p["value"] = strHex;

        params.append(p);

        if ( !queueRequest(RequestIPC(SendTransaction, "eth_sendTransaction", params)) ) {
            return bail();
        }
    }

    void EtherIPC::handleSendTransaction() {
        QJsonValue jv;
        if ( !readReply(jv) ) {
            return bail();
        }

        const QString hash = jv.toString();
        emit sendTransactionDone(hash);
        done();
    }

    int EtherIPC::getConnectionState() const {
        if ( fSocket.state() == QLocalSocket::ConnectedState ) {
            return 1; // TODO: add higher states per peer count!
        }

        return 0;
    }

    void EtherIPC::unlockAccount(const QString& hash, const QString& password, int duration, int index) {
        QJsonArray params;
        params.append(hash);
        params.append(password);

        BigInt::Vin vinVal(duration);
        QString strHex = QString(vinVal.toStr0xHex().data());
        params.append(strHex);

        if ( !queueRequest(RequestIPC(UnlockAccount, "personal_unlockAccount", params, index)) ) {
            return bail();
        }
    }

    void EtherIPC::handleUnlockAccount() {
        QJsonValue jv;
        if ( !readReply(jv) ) {
            return bail();
        }

        const bool result = jv.toBool(false);
        emit unlockAccountDone(result, fActiveRequest.getIndex());
        done();
    }

    const QString EtherIPC::getConnectionStateStr() const {
        switch ( getConnectionState() ) {
            case 0: return QStringLiteral("Disconnected");
            case 1: return QStringLiteral("Connected (poor peer count)"); // TODO: show peer count in str
            case 2: return QStringLiteral("Connected (fair peer count)");
            case 3: return QStringLiteral("Connected (good peer count)");
        default:
            return QStringLiteral("Invalid");
        }
    }

    void EtherIPC::getGasPrice() {
        if ( !queueRequest(RequestIPC(GetGasPrice, "eth_gasPrice")) ) {
            return bail();
        }
    }

    void EtherIPC::handleGetGasPrice() {
        QJsonValue jv;
        if ( !readReply(jv) ) {
            return bail();
        }

        const QString decStr = toDecStr(jv);

        emit getGasPriceDone(decStr);
        done();
    }

    quint64 EtherIPC::peerCount() const {
        return fPeerCount;
    }

    void EtherIPC::newPendingTransactionFilter() {
        if ( !queueRequest(RequestIPC(NewPendingTransactionFilter, "eth_newPendingTransactionFilter")) ) {
            return bail();
        }
    }

    void EtherIPC::handleNewPendingTransactionFilter() {
        if ( !readNumber(fPendingTransactionsFilterID) ) {
            return bail();
        }

        done();
    }

    void EtherIPC::bail() {
        fActiveRequest = RequestIPC();
        fRequestQueue.clear();
        emit error(fError, fCode);
        emit connectionStateChanged();
        done();
    }

    void EtherIPC::done() {
        if ( !fRequestQueue.isEmpty() ) {
            fActiveRequest = fRequestQueue.first();
            fRequestQueue.removeFirst();
            writeRequest();
        } else {
            fActiveRequest = RequestIPC();
            emit busyChanged(getBusy());
        }
    }

    QJsonObject EtherIPC::methodToJSON(const RequestIPC& request) {
        QJsonObject result;

        result.insert("jsonrpc", QJsonValue("2.0"));
        result.insert("method", QJsonValue(request.getMethod()));
        result.insert("id", QJsonValue(request.getCallID()));
        result.insert("params", QJsonValue(request.getParams()));

        return result;
    }

    bool EtherIPC::queueRequest(const RequestIPC& request) {
        if ( fActiveRequest.empty() ) {
            fActiveRequest = request;
            emit busyChanged(getBusy());
            return writeRequest();
        } else {
            fRequestQueue.append(request);
            return true;
        }
    }

    bool EtherIPC::writeRequest() {
        QJsonDocument doc(methodToJSON(fActiveRequest));
        const QString msg(doc.toJson());

        if ( !fSocket.isWritable() ) {
            fError = "Socket not writeable";
            fCode = 0;
            return false;
        }

        const QByteArray sendBuf = msg.toUtf8();
        const int sent = fSocket.write(sendBuf);

        if ( sent <= 0 ) {
            fError = "Error on socket write: " + fSocket.errorString();
            fCode = 0;
            return false;
        }

        //qDebug() << "sent: " << msg << "\n";

        return true;
    }

    bool EtherIPC::readReply(QJsonValue& result) {
        QByteArray recvBuf = fSocket.read(4096);
        if ( recvBuf.isNull() || recvBuf.isEmpty() ) {
            fError = "Error on socket read: " + fSocket.errorString();
            fCode = 0;
            return false;
        }

        //qDebug() << "received: " << recvBuf << "\n";

        QJsonParseError parseError;
        QJsonDocument resDoc = QJsonDocument::fromJson(recvBuf, &parseError);

        if ( parseError.error != QJsonParseError::NoError ) {
            fError = "Response parse error: " + parseError.errorString();
            fCode = 0;
            return false;
        }

        const QJsonObject obj = resDoc.object();
        const int objID = obj["id"].toInt(-1);

        if ( objID != fActiveRequest.getCallID() ) { // TODO
            fError = "Call number mismatch";
            fCode = 0;
            return false;
        }

        result = obj["result"];

        if ( result.isUndefined() || result.isNull() ) {
            if ( obj.contains("error") ) {
                if ( obj["error"].toObject().contains("message") ) {
                    fError = obj["error"].toObject()["message"].toString();
                }

                if ( obj["error"].toObject().contains("code") ) {
                    fCode = obj["error"].toObject()["code"].toInt();
                }

                return false;
            }

            fError = "Result object undefined in IPC response";
            return false;
        }

        return true;
    }

    bool EtherIPC::readNumber(quint64& result) {
        QJsonValue jv;
        if ( !readReply(jv) ) {
            return false;
        }

        std::string hexStr = jv.toString("0x0").remove(0, 2).toStdString();
        const BigInt::Vin bv(hexStr, 16);

        result = bv.toUlong();
        return true;
    }

    const QString EtherIPC::toDecStr(const QJsonValue& jv) const {
        std::string hexStr = jv.toString("0x0").remove(0, 2).toStdString();
        const BigInt::Vin bv(hexStr, 16);
        QString decStr = QString(bv.toStrDec().data());

        int dsl = decStr.length();
        if ( dsl <= 18 ) {
            decStr.prepend(QString(19 - dsl, '0'));
            dsl = decStr.length();
        }
        decStr.insert(dsl - 18, fLocale.decimalPoint());
        return decStr;
    }

    void EtherIPC::onSocketError(QLocalSocket::LocalSocketError err) {
        fError = fSocket.errorString();
        fCode = err;
    }

    void EtherIPC::onSocketReadyRead() {
        switch ( fActiveRequest.getType() ) {
        case NewAccount: {
                handleNewAccount();
                break;
            }
        case DeleteAccount: {
                handleDeleteAccount();
                break;
            }
        case GetBlockNumber: {
                handleGetBlockNumber();
                break;
            }
        case GetAccountRefs: {
                handleAccountDetails();
                break;
            }
        case GetBalance: {
                handleAccountBalance();
                break;
            }
        case GetTransactionCount: {
                handleAccountTransactionCount();
                break;
            }
        case SendTransaction: {
                handleSendTransaction();
                break;
            }
        case UnlockAccount: {
                handleUnlockAccount();
                break;
            }
        case GetGasPrice: {
                handleGetGasPrice();
                break;
            }
        case NewPendingTransactionFilter: {
                handleNewPendingTransactionFilter();
                break;
            }
        default: break;
        }
    }

}