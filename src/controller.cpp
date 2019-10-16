#include "controller.h"

#include "addressbook.h"
#include "settings.h"
#include "senttxstore.h"
#include "version.h"
#include "websockets.h"

using json = nlohmann::json;

Controller::Controller(MainWindow* main) {
    auto cl = new ConnectionLoader(main, this);

    // Execute the load connection async, so we can set up the rest of RPC properly. 
    QTimer::singleShot(1, [=]() { cl->loadConnection(); });

    this->main = main;
    this->ui = main->ui;

    // Setup balances table model
    balancesTableModel = new BalancesTableModel(main->ui->balancesTable);
    main->ui->balancesTable->setModel(balancesTableModel);

    // Setup transactions table model
    transactionsTableModel = new TxTableModel(ui->transactionsTable);
    main->ui->transactionsTable->setModel(transactionsTableModel);
    
    // Set up timer to refresh Price
    priceTimer = new QTimer(main);
    QObject::connect(priceTimer, &QTimer::timeout, [=]() {
        if (Settings::getInstance()->getAllowFetchPrices())
            refreshZECPrice();
    });
    priceTimer->start(Settings::priceRefreshSpeed);  // Every hour

    // Set up a timer to refresh the UI every few seconds
    timer = new QTimer(main);
    QObject::connect(timer, &QTimer::timeout, [=]() {
        refresh();
    });
    timer->start(Settings::updateSpeed);    

    // Set up the timer to watch for tx status
    txTimer = new QTimer(main);
    QObject::connect(txTimer, &QTimer::timeout, [=]() {
        watchTxStatus();
    });
    // Start at every 10s. When an operation is pending, this will change to every second
    txTimer->start(Settings::updateSpeed);  

    // Create the data model
    model = new DataModel();

    // Crate the ZcashdRPC 
    zrpc = new LiteInterface();
}

Controller::~Controller() {
    delete timer;
    delete txTimer;

    delete transactionsTableModel;
    delete balancesTableModel;

    delete model;
    delete zrpc;
}


// Called when a connection to zcashd is available. 
void Controller::setConnection(Connection* c) {
    if (c == nullptr) return;

    this->zrpc->setConnection(c);

    ui->statusBar->showMessage("Ready!");

    // See if we need to remove the reindex/rescan flags from the zcash.conf file
    auto zcashConfLocation = Settings::getInstance()->getZcashdConfLocation();
    Settings::removeFromZcashConf(zcashConfLocation, "rescan");
    Settings::removeFromZcashConf(zcashConfLocation, "reindex");

    // If we're allowed to get the Zec Price, get the prices
    if (Settings::getInstance()->getAllowFetchPrices())
        refreshZECPrice();

    // If we're allowed to check for updates, check for a new release
    if (Settings::getInstance()->getCheckForUpdates())
        checkForUpdate();

    // Force update, because this might be coming from a settings update
    // where we need to immediately refresh
    refresh(true);
}


// Build the RPC JSON Parameters for this tx
void Controller::fillTxJsonParams(json& params, Tx tx) {   
    Q_ASSERT(params.is_array());
    // Get all the addresses and amounts
    json allRecepients = json::array();

    // For each addr/amt/memo, construct the JSON and also build the confirm dialog box    
    for (int i=0; i < tx.toAddrs.size(); i++) {
        auto toAddr = tx.toAddrs[i];

        // Construct the JSON params
        json rec = json::object();
        rec["address"]      = toAddr.addr.toStdString();
        // Force it through string for rounding. Without this, decimal points beyond 8 places
        // will appear, causing an "invalid amount" error
        rec["amount"]       = Settings::getDecimalString(toAddr.amount).toStdString(); //.toDouble(); 
        if (Settings::isZAddress(toAddr.addr) && !toAddr.encodedMemo.trimmed().isEmpty())
            rec["memo"]     = toAddr.encodedMemo.toStdString();

        allRecepients.push_back(rec);
    }

    // Add sender    
    params.push_back(tx.fromAddr.toStdString());
    params.push_back(allRecepients);

    // Add fees if custom fees are allowed.
    if (Settings::getInstance()->getAllowCustomFees()) {
        params.push_back(1); // minconf
        params.push_back(tx.fee);
    }
}


void Controller::noConnection() {    
    QIcon i = QApplication::style()->standardIcon(QStyle::SP_MessageBoxCritical);
    main->statusIcon->setPixmap(i.pixmap(16, 16));
    main->statusIcon->setToolTip("");
    main->statusLabel->setText(QObject::tr("No Connection"));
    main->statusLabel->setToolTip("");
    main->ui->statusBar->showMessage(QObject::tr("No Connection"), 1000);

    // Clear balances table.
    QMap<QString, qint64> emptyBalances;
    QList<UnspentOutput>  emptyOutputs;
    balancesTableModel->setNewData(emptyBalances, emptyOutputs);

    // Clear Transactions table.
    QList<TransactionItem> emptyTxs;
    transactionsTableModel->addTData(emptyTxs);
    transactionsTableModel->addZRecvData(emptyTxs);
    transactionsTableModel->addZSentData(emptyTxs);

    // Clear balances
    ui->balSheilded->setText("");
    ui->balTransparent->setText("");
    ui->balTotal->setText("");

    ui->balSheilded->setToolTip("");
    ui->balTransparent->setToolTip("");
    ui->balTotal->setToolTip("");

    // Clear send tab from address
    ui->inputsCombo->clear();
}

// Refresh received z txs by calling z_listreceivedbyaddress/gettransaction
void Controller::refreshReceivedZTrans(QList<QString> zaddrs) {
    if (!zrpc->haveConnection()) 
        return noConnection();

    // We'll only refresh the received Z txs if settings allows us.
    if (!Settings::getInstance()->getSaveZtxs()) {
        QList<TransactionItem> emptylist;
        transactionsTableModel->addZRecvData(emptylist);
        return;
    }
        
    zrpc->fetchReceivedZTrans(zaddrs, 
    [=] (QString addr) {
        model->markAddressUsed(addr);
    },
    [=] (QList<TransactionItem> txdata) {
        transactionsTableModel->addZRecvData(txdata);
    }
    );
} 

/// This will refresh all the balance data from zcashd
void Controller::refresh(bool force) {
    if (!zrpc->haveConnection()) 
        return noConnection();

    getInfoThenRefresh(force);
}


void Controller::getInfoThenRefresh(bool force) {
    if (!zrpc->haveConnection()) 
        return noConnection();

    static bool prevCallSucceeded = false;

    zrpc->fetchInfo([=] (const json& reply) {   
        qDebug() << "Info updated";

        prevCallSucceeded = true;

        // Testnet?
        if (!reply["chain_name"].is_null()) {
            Settings::getInstance()->setTestnet(reply["chain_name"].get<json::string_t>() == "test");
        };

        // Recurring pamynets are testnet only
        if (!Settings::getInstance()->isTestnet())
            main->disableRecurring();

        // Connected, so display checkmark.
        QIcon i(":/icons/res/connected.gif");
        main->statusIcon->setPixmap(i.pixmap(16, 16));

        static int    lastBlock = 0;
        int curBlock  = reply["latest_block_height"].get<json::number_integer_t>();
        //int version = reply["version"].get<json::string_t>();
        int version = 1;
        Settings::getInstance()->setZcashdVersion(version);

        // See if recurring payments needs anything
        Recurring::getInstance()->processPending(main);

        if ( force || (curBlock != lastBlock) ) {
            // Something changed, so refresh everything.
            lastBlock = curBlock;

            refreshBalances();        
            refreshAddresses();     // This calls refreshZSentTransactions() and refreshReceivedZTrans()
            refreshTransactions();
        }
    }, [=](QString err) {
        // zcashd has probably disappeared.
        this->noConnection();

        // Prevent multiple dialog boxes, because these are called async
        static bool shown = false;
        if (!shown && prevCallSucceeded) { // show error only first time
            shown = true;
            QMessageBox::critical(main, QObject::tr("Connection Error"), QObject::tr("There was an error connecting to zcashd. The error was") + ": \n\n"
                + err, QMessageBox::StandardButton::Ok);
            shown = false;
        }

        prevCallSucceeded = false;
    });
}

void Controller::refreshAddresses() {
    if (!zrpc->haveConnection()) 
        return noConnection();
    
    auto newzaddresses = new QList<QString>();

    zrpc->fetchZAddresses([=] (json reply) {
        for (auto& it : reply.get<json::array_t>()) {   
            auto addr = QString::fromStdString(it.get<json::string_t>());
            newzaddresses->push_back(addr);
        }

        model->replaceZaddresses(newzaddresses);

        // Refresh the sent and received txs from all these z-addresses
        refreshSentZTrans();
        refreshReceivedZTrans(model->getAllZAddresses());
    });

    
    auto newtaddresses = new QList<QString>();
    zrpc->fetchTAddresses([=] (json reply) {
        for (auto& it : reply.get<json::array_t>()) {   
            auto addr = QString::fromStdString(it.get<json::string_t>());
            if (Settings::isTAddress(addr))
                newtaddresses->push_back(addr);
        }

        model->replaceTaddresses(newtaddresses);
    });
}

// Function to create the data model and update the views, used below.
void Controller::updateUI(bool anyUnconfirmed) {    
    ui->unconfirmedWarning->setVisible(anyUnconfirmed);

    // Update balances model data, which will update the table too
    balancesTableModel->setNewData(model->getAllBalances(), model->getUTXOs());

    // Update from address
    main->updateFromCombo();
};

// Function to process reply of the listunspent and z_listunspent API calls, used below.
bool Controller::processUnspent(const json& reply, QMap<QString, qint64>* balancesMap, QList<UnspentOutput>* newUtxos) {
    bool anyUnconfirmed = false;

    auto processFn = [=](const json& array) {
        for (auto& it : array) {
            QString qsAddr  = QString::fromStdString(it["address"]);
            int block       = it["created_in_block"].get<json::number_unsigned_t>();
            QString txid    = QString::fromStdString(it["created_in_txid"]);
            QString amount  = Settings::getDecimalString(it["value"].get<json::number_unsigned_t>());

            newUtxos->push_back(UnspentOutput{ qsAddr, txid, amount, block, true });

            (*balancesMap)[qsAddr] = (*balancesMap)[qsAddr] + it["value"].get<json::number_unsigned_t>();
        }    
    };

    processFn(reply["unspent_notes"].get<json::array_t>());
    processFn(reply["utxos"].get<json::array_t>());

    return anyUnconfirmed;
};

void Controller::refreshBalances() {    
    if (!zrpc->haveConnection()) 
        return noConnection();

    // 1. Get the Balances
    zrpc->fetchBalance([=] (json reply) {    
        auto balT      = reply["tbalance"].get<json::number_unsigned_t>();
        auto balZ      = reply["zbalance"].get<json::number_unsigned_t>();
        auto balTotal  = balT + balZ;

        AppDataModel::getInstance()->setBalances(balT, balZ);

        ui->balSheilded   ->setText(Settings::getZECDisplayFormat(balZ));
        ui->balTransparent->setText(Settings::getZECDisplayFormat(balT));
        ui->balTotal      ->setText(Settings::getZECDisplayFormat(balTotal));


        ui->balSheilded   ->setToolTip(Settings::getZECDisplayFormat(balZ));
        ui->balTransparent->setToolTip(Settings::getZECDisplayFormat(balT));
        ui->balTotal      ->setToolTip(Settings::getZECDisplayFormat(balTotal));
    });

    // 2. Get the UTXOs
    // First, create a new UTXO list. It will be replacing the existing list when everything is processed.
    auto newUtxos = new QList<UnspentOutput>();
    auto newBalances = new QMap<QString, qint64>();

    // Call the Transparent and Z unspent APIs serially and then, once they're done, update the UI
    zrpc->fetchUnspent([=] (json reply) {
        auto anyUnconfirmed = processUnspent(reply, newBalances, newUtxos);

        // Swap out the balances and UTXOs
        model->replaceBalances(newBalances);
        model->replaceUTXOs(newUtxos);

        updateUI(anyUnconfirmed);

        main->balancesReady();
    });
}

void Controller::refreshTransactions() {    
    if (!zrpc->haveConnection()) 
        return noConnection();

    zrpc->fetchTransactions([=] (json reply) {
        QList<TransactionItem> txdata;

        for (auto& it : reply.get<json::array_t>()) {  
            double fee = 0;
            if (!it["fee"].is_null()) {
                fee = it["fee"].get<json::number_float_t>();
            }

            QString address = (it["address"].is_null() ? "" : QString::fromStdString(it["address"]));

            TransactionItem tx{
                QString::fromStdString(it["category"]),
                (qint64)it["time"].get<json::number_unsigned_t>(),
                address,
                QString::fromStdString(it["txid"]),
                it["amount"].get<json::number_float_t>() + fee,
                static_cast<long>(it["confirmations"].get<json::number_unsigned_t>()),
                "", "" };

            txdata.push_back(tx);
            if (!address.isEmpty())
                model->markAddressUsed(address);
        }

        // Update model data, which updates the table view
        transactionsTableModel->addTData(txdata);        
    });
}

// Read sent Z transactions from the file.
void Controller::refreshSentZTrans() {
    if (!zrpc->haveConnection()) 
        return noConnection();

    auto sentZTxs = SentTxStore::readSentTxFile();

    // If there are no sent z txs, then empty the table. 
    // This happens when you clear history.
    if (sentZTxs.isEmpty()) {
        transactionsTableModel->addZSentData(sentZTxs);
        return;
    }

    QList<QString> txids;

    for (auto sentTx: sentZTxs) {
        txids.push_back(sentTx.txid);
    }

    // Look up all the txids to get the confirmation count for them. 
    zrpc->fetchReceivedTTrans(txids, sentZTxs, [=](auto newSentZTxs) {
        transactionsTableModel->addZSentData(newSentZTxs);
    });
}

void Controller::addNewTxToWatch(const QString& newOpid, WatchedTx wtx) {    
    watchingOps.insert(newOpid, wtx);

    watchTxStatus();
}

/**
 * Execute a transaction with the standard UI. i.e., standard status bar message and standard error
 * handling
 */
void Controller::executeStandardUITransaction(Tx tx) {
    executeTransaction(tx, 
        [=] (QString opid) {
            ui->statusBar->showMessage(QObject::tr("Computing Tx: ") % opid);
        },
        [=] (QString, QString txid) { 
            ui->statusBar->showMessage(Settings::txidStatusMessage + " " + txid);
        },
        [=] (QString opid, QString errStr) {
            ui->statusBar->showMessage(QObject::tr(" Tx ") % opid % QObject::tr(" failed"), 15 * 1000);

            if (!opid.isEmpty())
                errStr = QObject::tr("The transaction with id ") % opid % QObject::tr(" failed. The error was") + ":\n\n" + errStr; 

            QMessageBox::critical(main, QObject::tr("Transaction Error"), errStr, QMessageBox::Ok);            
        }
    );
}


// Execute a transaction!
void Controller::executeTransaction(Tx tx, 
        const std::function<void(QString opid)> submitted,
        const std::function<void(QString opid, QString txid)> computed,
        const std::function<void(QString opid, QString errStr)> error) {
    // First, create the json params
    json params = json::array();
    fillTxJsonParams(params, tx);
    std::cout << std::setw(2) << params << std::endl;

    zrpc->sendZTransaction(params, [=](const json& reply) {
        QString opid = QString::fromStdString(reply.get<json::string_t>());

        // And then start monitoring the transaction
        addNewTxToWatch( opid, WatchedTx { opid, tx, computed, error} );
        submitted(opid);
    },
    [=](QString errStr) {
        error("", errStr);
    });
}


void Controller::watchTxStatus() {
    if (!zrpc->haveConnection()) 
        return noConnection();

    zrpc->fetchOpStatus([=] (const json& reply) {
        // There's an array for each item in the status
        for (auto& it : reply.get<json::array_t>()) {  
            // If we were watching this Tx and its status became "success", then we'll show a status bar alert
            QString id = QString::fromStdString(it["id"]);
            if (watchingOps.contains(id)) {
                // And if it ended up successful
                QString status = QString::fromStdString(it["status"]);
                main->loadingLabel->setVisible(false);

                if (status == "success") {
                    auto txid = QString::fromStdString(it["result"]["txid"]);
                    
                    SentTxStore::addToSentTx(watchingOps[id].tx, txid);

                    auto wtx = watchingOps[id];
                    watchingOps.remove(id);
                    wtx.completed(id, txid);

                    // Refresh balances to show unconfirmed balances                    
                    refresh(true);
                } else if (status == "failed") {
                    // If it failed, then we'll actually show a warning. 
                    auto errorMsg = QString::fromStdString(it["error"]["message"]);

                    auto wtx = watchingOps[id];
                    watchingOps.remove(id);
                    wtx.error(id, errorMsg);
                } 
            }

            if (watchingOps.isEmpty()) {
                txTimer->start(Settings::updateSpeed);
            } else {
                txTimer->start(Settings::quickUpdateSpeed);
            }
        }

        // If there is some op that we are watching, then show the loading bar, otherwise hide it
        if (watchingOps.empty()) {
            main->loadingLabel->setVisible(false);
        } else {
            main->loadingLabel->setVisible(true);
            main->loadingLabel->setToolTip(QString::number(watchingOps.size()) + QObject::tr(" tx computing. This can take several minutes."));
        }
    });
}

void Controller::checkForUpdate(bool silent) {
    if (!zrpc->haveConnection()) 
        return noConnection();

    QUrl cmcURL("https://api.github.com/repos/ZcashFoundation/zecwallet/releases");

    QNetworkRequest req;
    req.setUrl(cmcURL);
    
    QNetworkAccessManager *manager = new QNetworkAccessManager(this->main);
    QNetworkReply *reply = manager->get(req);

    QObject::connect(reply, &QNetworkReply::finished, [=] {
        reply->deleteLater();
        manager->deleteLater();

        try {
            if (reply->error() == QNetworkReply::NoError) {

                auto releases = QJsonDocument::fromJson(reply->readAll()).array();
                QVersionNumber maxVersion(0, 0, 0);

                for (QJsonValue rel : releases) {
                    if (!rel.toObject().contains("tag_name"))
                        continue;

                    QString tag = rel.toObject()["tag_name"].toString();
                    if (tag.startsWith("v"))
                        tag = tag.right(tag.length() - 1);

                    if (!tag.isEmpty()) {
                        auto v = QVersionNumber::fromString(tag);
                        if (v > maxVersion)
                            maxVersion = v;
                    }
                }

                auto currentVersion = QVersionNumber::fromString(APP_VERSION);
                
                // Get the max version that the user has hidden updates for
                QSettings s;
                auto maxHiddenVersion = QVersionNumber::fromString(s.value("update/lastversion", "0.0.0").toString());

                qDebug() << "Version check: Current " << currentVersion << ", Available " << maxVersion;

                if (maxVersion > currentVersion && (!silent || maxVersion > maxHiddenVersion)) {
                    auto ans = QMessageBox::information(main, QObject::tr("Update Available"), 
                        QObject::tr("A new release v%1 is available! You have v%2.\n\nWould you like to visit the releases page?")
                            .arg(maxVersion.toString())
                            .arg(currentVersion.toString()),
                        QMessageBox::Yes, QMessageBox::Cancel);
                    if (ans == QMessageBox::Yes) {
                        QDesktopServices::openUrl(QUrl("https://github.com/ZcashFoundation/zecwallet/releases"));
                    } else {
                        // If the user selects cancel, don't bother them again for this version
                        s.setValue("update/lastversion", maxVersion.toString());
                    }
                } else {
                    if (!silent) {
                        QMessageBox::information(main, QObject::tr("No updates available"), 
                            QObject::tr("You already have the latest release v%1")
                                .arg(currentVersion.toString()));
                    }
                } 
            }
        }
        catch (...) {
            // If anything at all goes wrong, just set the price to 0 and move on.
            qDebug() << QString("Caught something nasty");
        }       
    });
}

// Get the ZEC->USD price from coinmarketcap using their API
void Controller::refreshZECPrice() {
    if (!zrpc->haveConnection()) 
        return noConnection();

    QUrl cmcURL("https://api.coinmarketcap.com/v1/ticker/");

    QNetworkRequest req;
    req.setUrl(cmcURL);
    
    QNetworkAccessManager *manager = new QNetworkAccessManager(this->main);
    QNetworkReply *reply = manager->get(req);

    QObject::connect(reply, &QNetworkReply::finished, [=] {
        reply->deleteLater();
        manager->deleteLater();

        try {
            if (reply->error() != QNetworkReply::NoError) {
                auto parsed = json::parse(reply->readAll(), nullptr, false);
                if (!parsed.is_discarded() && !parsed["error"]["message"].is_null()) {
                    qDebug() << QString::fromStdString(parsed["error"]["message"]);    
                } else {
                    qDebug() << reply->errorString();
                }
                Settings::getInstance()->setZECPrice(0);
                return;
            } 

            auto all = reply->readAll();
            
            auto parsed = json::parse(all, nullptr, false);
            if (parsed.is_discarded()) {
                Settings::getInstance()->setZECPrice(0);
                return;
            }

            for (const json& item : parsed.get<json::array_t>()) {
                if (item["symbol"].get<json::string_t>() == Settings::getTokenName().toStdString()) {
                    QString price = QString::fromStdString(item["price_usd"].get<json::string_t>());
                    qDebug() << Settings::getTokenName() << " Price=" << price;
                    Settings::getInstance()->setZECPrice(price.toDouble());

                    return;
                }
            }
        } catch (...) {
            // If anything at all goes wrong, just set the price to 0 and move on.
            qDebug() << QString("Caught something nasty");
        }

        // If nothing, then set the price to 0;
        Settings::getInstance()->setZECPrice(0);
    });
}

void Controller::shutdownZcashd() {
    // Shutdown embedded zcashd if it was started
    if (ezcashd == nullptr || ezcashd->processId() == 0 || !zrpc->haveConnection()) {
        // No zcashd running internally, just return
        return;
    }

    // json payload = {
    //     {"jsonrpc", "1.0"},
    //     {"id", "someid"},
    //     {"method", "stop"}
    // };
    
    // getConnection()->doRPCWithDefaultErrorHandling(payload, [=](auto) {});
    // getConnection()->shutdown();

    // QDialog d(main);
    // Ui_ConnectionDialog connD;
    // connD.setupUi(&d);
    // connD.topIcon->setBasePixmap(QIcon(":/icons/res/icon.ico").pixmap(256, 256));
    // connD.status->setText(QObject::tr("Please wait for ZecWallet to exit"));
    // connD.statusDetail->setText(QObject::tr("Waiting for zcashd to exit"));

    // QTimer waiter(main);

    // // We capture by reference all the local variables because of the d.exec() 
    // // below, which blocks this function until we exit. 
    // int waitCount = 0;
    // QObject::connect(&waiter, &QTimer::timeout, [&] () {
    //     waitCount++;

    //     if ((ezcashd->atEnd() && ezcashd->processId() == 0) ||
    //         waitCount > 30 || 
    //         getConnection()->config->zcashDaemon)  {   // If zcashd is daemon, then we don't have to do anything else
    //         qDebug() << "Ended";
    //         waiter.stop();
    //         QTimer::singleShot(1000, [&]() { d.accept(); });
    //     } else {
    //         qDebug() << "Not ended, continuing to wait...";
    //     }
    // });
    // waiter.start(1000);

    // // Wait for the zcash process to exit.
    // if (!Settings::getInstance()->isHeadless()) {
    //     d.exec(); 
    // } else {
    //     while (waiter.isActive()) {
    //         QCoreApplication::processEvents();

    //         QThread::sleep(1);
    //     }
    // }
}


// // Fetch the Z-board topics list
// void Controller::getZboardTopics(std::function<void(QMap<QString, QString>)> cb) {
//     if (!zrpc->haveConnection())
//         return noConnection();

//     QUrl cmcURL("http://z-board.net/listTopics");

//     QNetworkRequest req;
//     req.setUrl(cmcURL);

//     QNetworkReply *reply = conn->restclient->get(req);

//     QObject::connect(reply, &QNetworkReply::finished, [=] {
//         reply->deleteLater();

//         try {
//             if (reply->error() != QNetworkReply::NoError) {
//                 auto parsed = json::parse(reply->readAll(), nullptr, false);
//                 if (!parsed.is_discarded() && !parsed["error"]["message"].is_null()) {
//                     qDebug() << QString::fromStdString(parsed["error"]["message"]);
//                 }
//                 else {
//                     qDebug() << reply->errorString();
//                 }
//                 return;
//             }

//             auto all = reply->readAll();

//             auto parsed = json::parse(all, nullptr, false);
//             if (parsed.is_discarded()) {
//                 return;
//             }

//             QMap<QString, QString> topics;
//             for (const json& item : parsed["topics"].get<json::array_t>()) {
//                 if (item.find("addr") == item.end() || item.find("topicName") == item.end())
//                     return;

//                 QString addr  = QString::fromStdString(item["addr"].get<json::string_t>());
//                 QString topic = QString::fromStdString(item["topicName"].get<json::string_t>());
                
//                 topics.insert(topic, addr);
//             }

//             cb(topics);
//         }
//         catch (...) {
//             // If anything at all goes wrong, just set the price to 0 and move on.
//             qDebug() << QString("Caught something nasty");
//         }
//     });
// }

/** 
 * Get a Sapling address from the user's wallet
 */ 
QString Controller::getDefaultSaplingAddress() {
    for (QString addr: model->getAllZAddresses()) {
        if (Settings::getInstance()->isSaplingAddress(addr))
            return addr;
    }

    return QString();
}

QString Controller::getDefaultTAddress() {
    if (model->getAllTAddresses().length() > 0)
        return model->getAllTAddresses().at(0);
    else 
        return QString();
}
