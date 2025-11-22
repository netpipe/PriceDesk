// main.cpp
#include <QApplication>
#include <QWidget>
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QAction>
#include <QDialog>
#include <QFormLayout>
#include <QLineEdit>
#include <QSpinBox>
#include <QPushButton>
#include <QSettings>
#include <QMouseEvent>
#include <QPainter>
#include <QDateTime>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QPlainTextEdit>
#include <QLabel>
#include <QScreen>

static QString apiSimplePrice(const QString& ids, const QString& vs_currencies) {
    return QString("https://api.coingecko.com/api/v3/simple/price?ids=%1&vs_currencies=%2")
            .arg(ids, vs_currencies);
}
static QString apiMarketChart(const QString& id, const QString& vs_currency, int days) {
    return QString("https://api.coingecko.com/api/v3/coins/%1/market_chart?vs_currency=%2&days=%3")
            .arg(id, vs_currency).arg(days);
}

// Simple lightweight chart widget (draws a line chart)
class MiniChart : public QWidget {
    Q_OBJECT
public:
    MiniChart(QWidget* parent=nullptr) : QWidget(parent) {
        setMinimumHeight(120);
    }

    // input: vector of [timestamp, price] pairs (timestamp in ms)
    void setData(const QVector<QPair<qint64,double>>& d) {
        data = d;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), palette().window());
        if (data.isEmpty()) {
            p.setPen(Qt::gray);
            p.drawText(rect(), Qt::AlignCenter, "No chart data");
            return;
        }

        // find min/max
        double minv = data[0].second, maxv = data[0].second;
        qint64 minT = data.first().first, maxT = data.last().first;
        for (auto &pt : data) {
            minv = qMin(minv, pt.second);
            maxv = qMax(maxv, pt.second);
        }
        if (qFuzzyCompare(minv, maxv)) {
            minv *= 0.999; maxv *= 1.001;
        }

        QRectF area = rect().adjusted(8,8,-8,-8);
        QPen pen(Qt::black);
        pen.setWidth(2);
        p.setPen(pen);

        // Map to points
        QVector<QPointF> pts;
        for (auto &pt : data) {
            double tnorm = double(pt.first - minT) / double(maxT - minT);
            double vnorm = (pt.second - minv) / (maxv - minv);
            double x = area.left() + tnorm * area.width();
            double y = area.bottom() - vnorm * area.height();
            pts.append(QPointF(x,y));
        }

        // draw polyline
        for (int i=1;i<pts.size();++i) {
            p.drawLine(pts[i-1], pts[i]);
        }

        // draw axes labels (min/max)
        p.setPen(Qt::gray);
        p.drawText(area.left(), area.bottom()+12, QString::number(minv,'f',6));
        p.drawText(area.left(), area.top()-2, QString::number(maxv,'f',6));
    }

private:
    QVector<QPair<qint64,double>> data;
};

// Overlay widget showing multiple currency lines
class PriceOverlay : public QWidget {
    Q_OBJECT
public:
    PriceOverlay(QNetworkAccessManager* mgr, QWidget* parent=nullptr)
        : QWidget(parent), manager(mgr)
    {
        setWindowFlags(Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);

        QVBoxLayout* l = new QVBoxLayout(this);
        l->setContentsMargins(10,10,10,10);
        container = new QWidget(this);
        QVBoxLayout* cl = new QVBoxLayout(container);
        cl->setContentsMargins(0,0,0,0);
        container->setLayout(cl);
        l->addWidget(container);

        setStyleSheet("QWidget { background: rgba(0,0,0,120); border-radius:8px; }"
                      "QLabel { color: white; }");

        // defaults

        QSettings s("Demo", "CryptoOverlay");
        coinIds = QStringList() << s.value("coins", "dogecoin").toString();
        vsCurrencies = QStringList()  << s.value("vs", "usd").toString();

        refreshMs = 30000; //30 seconds check

        timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, &PriceOverlay::fetchPrices);
        timer->start(refreshMs);

        // initial layout
        rebuildLabels();
    }

    void setCoins(const QStringList& coins) {
        coinIds = coins;
        rebuildLabels();
        fetchPrices();
    }
    QStringList coins() const { return coinIds; }

    void setVsCurrencies(const QStringList& vs) {
        vsCurrencies = vs;
        rebuildLabels();
        fetchPrices();
    }
    QStringList vs() const { return vsCurrencies; }

    void setRefreshInterval(int ms) {
        refreshMs = ms;
        timer->start(refreshMs);
    }
    int refreshInterval() const { return refreshMs; }

    // alarms lines format: each line "coin,currency,threshold"
    void setAlarmLines(const QStringList& lines) {
        alarms.clear();
        for (const QString& ln : lines) {
            QStringList parts = ln.split(',', QString::SkipEmptyParts);
            if (parts.size() < 3) continue;
            QString coin = parts[0].trimmed().toLower();
            QString cur = parts[1].trimmed().toLower();
            bool ok=false;
            double th = parts[2].trimmed().toDouble(&ok);
            if (ok) alarms.append({coin,cur,th});
        }
    }
    QStringList alarmLines() const {
        QStringList out;
        for (auto &a : alarms) out << QString("%1,%2,%3").arg(a.coin).arg(a.currency).arg(a.threshold);
        return out;
    }

    // show chart for first coin/currency
    void requestChart(int days = 2) {
        if (coinIds.isEmpty() || vsCurrencies.isEmpty()) return;
        QString id = coinIds.first();
        QString vs = vsCurrencies.first();
        QString url = apiMarketChart(id, vs, days);
        QNetworkRequest req{ QUrl(url) };
        auto reply = manager->get(QNetworkRequest(QUrl(url)));
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            if (reply->error() != QNetworkReply::NoError) {
                // ignore
                reply->deleteLater();
                emit chartDataReady(QVector<QPair<qint64,double>>());
                return;
            }
            QByteArray b = reply->readAll();
            QJsonDocument doc = QJsonDocument::fromJson(b);
            QJsonObject obj = doc.object();
            QVector<QPair<qint64,double>> data;
            if (obj.contains("prices") && obj["prices"].isArray()) {
                QJsonArray arr = obj["prices"].toArray();
                for (auto v : arr) {
                    if (!v.isArray()) continue;
                    QJsonArray pair = v.toArray();
                    if (pair.size() < 2) continue;
                    qint64 t = qint64(pair[0].toDouble());
                    double price = pair[1].toDouble();
                    data.append({t, price});
                }
            }
            reply->deleteLater();
            emit chartDataReady(data);
        });
    }

signals:
    void alarmTriggered(const QString& message);
    void chartDataReady(const QVector<QPair<qint64,double>>&);

protected:
    // draggable overlay
    void mousePressEvent(QMouseEvent* ev) override {
        if (ev->button() == Qt::LeftButton) {
            dragging = true;
            dragOffset = ev->globalPos() - frameGeometry().topLeft();
        }
        QWidget::mousePressEvent(ev);
    }
    void mouseMoveEvent(QMouseEvent* ev) override {
        if (dragging) {
            move(ev->globalPos() - dragOffset);
        }
        QWidget::mouseMoveEvent(ev);
    }
    void mouseReleaseEvent(QMouseEvent* ev) override {
        dragging = false;
        // emit save position via signal? we'll just let main read pos
        QWidget::mouseReleaseEvent(ev);
    }

private slots:
    void fetchPrices() {
        if (coinIds.isEmpty() || vsCurrencies.isEmpty()) return;

        // build comma lists
        QString ids = coinIds.join(",");
        QString vs = vsCurrencies.join(",");

        QString url = apiSimplePrice(ids, vs);
        QNetworkRequest req{ QUrl(url) };
        auto reply = manager->get(QNetworkRequest(QUrl(url)));
        connect(reply, &QNetworkReply::finished, this, [this, reply, ids, vs]() {
            if (reply->error() != QNetworkReply::NoError) {
                // show error on labels
                for (auto lbl : labelMatrix) {
                    lbl->setText("Error");
                }
                reply->deleteLater();
                return;
            }
            QByteArray b = reply->readAll();
            QJsonDocument doc = QJsonDocument::fromJson(b);
            QJsonObject root = doc.object();

            // iterate coins -> currencies
            for (int ci=0; ci<coinIds.size(); ++ci) {
                QString coin = coinIds[ci];
                QJsonObject coinObj = root.value(coin).toObject();
                for (int vi=0; vi<vsCurrencies.size(); ++vi) {
                    QString cur = vsCurrencies[vi];
                    double price = coinObj.value(cur).toDouble(std::numeric_limits<double>::quiet_NaN());
                    int idx = ci * vsCurrencies.size() + vi;
                    if (idx >= labelMatrix.size()) continue;
                    QLabel* lbl = labelMatrix[idx];
                    if (qIsNaN(price)) lbl->setText(QString("%1 (%2): -").arg(coin, cur.toUpper()));
                    else lbl->setText(QString("%1 (%2): %3").arg(coin, cur.toUpper()).arg(price));
                    // check alarms
                    for (auto &a : alarms) {
                        if (a.coin == coin && a.currency == cur) {
                            if (price >= a.threshold) {
                                QString msg = QString("%1 %2 reached %3 (threshold %4)")
                                              .arg(coin, cur.toUpper()).arg(price).arg(a.threshold);
                                emit alarmTriggered(msg);
                            }
                        }
                    }
                }
            }
            reply->deleteLater();
        });
    }

private:
    void rebuildLabels() {
        // clear
        QLayoutItem* child;
        QLayout* cl = container->layout();
        while ((child = cl->takeAt(0)) != nullptr) {
            QWidget* w = child->widget();
            if (w) w->deleteLater();
            delete child;
        }
        labelMatrix.clear();

        // create labels grid: coin-major, currencies-minor (vertical)
        for (const QString& coin : coinIds) {
            for (const QString& cur : vsCurrencies) {
                QLabel* lbl = new QLabel(QString("%1 (%2): ...").arg(coin).arg(cur.toUpper()), this);
                lbl->setStyleSheet("color:white; font-weight:bold; font-size:14px;");
                cl->addWidget(lbl);
                labelMatrix.append(lbl);
            }
        }
        // add small hint
        QLabel* hint = new QLabel("Drag to move. Right-click tray for options.", this);
        hint->setStyleSheet("color:rgba(255,255,255,0.7); font-size:11px;");
        cl->addWidget(hint);
    }

    struct Alarm { QString coin; QString currency; double threshold; };
    QStringList coinIds;
    QStringList vsCurrencies;
    QVector<QLabel*> labelMatrix;
    QWidget* container;
    QTimer* timer;
    int refreshMs;
    QNetworkAccessManager* manager;
    bool dragging=false;
    QPoint dragOffset;
    QVector<Alarm> alarms;
};

// Simple config dialog that edits settings and shows mini chart
class ConfigDialog : public QDialog {
    Q_OBJECT
public:
    ConfigDialog(PriceOverlay* overlay, QNetworkAccessManager* mgr, QWidget* parent=nullptr)
        : QDialog(parent), overlay(overlay), manager(mgr)
    {
        setWindowTitle("Widget Settings");
        setModal(false);

        QVBoxLayout* main = new QVBoxLayout(this);

        QFormLayout* form = new QFormLayout();
        coinEdit = new QLineEdit(overlay->coins().join(","));
        vsEdit   = new QLineEdit(overlay->vs().join(","));
        refreshSpin = new QSpinBox();
        refreshSpin->setRange(10000, 3600000);
        refreshSpin->setSingleStep(5000);
        refreshSpin->setValue(overlay->refreshInterval());
        posXSpin = new QSpinBox(); posYSpin = new QSpinBox();
        posXSpin->setRange(-10000, 10000); posYSpin->setRange(-10000,10000);
        QPoint p = overlay->pos();
        posXSpin->setValue(p.x());
        posYSpin->setValue(p.y());

        form->addRow("Coins (comma):", coinEdit);
        form->addRow("Vs Currencies (comma):", vsEdit);
        form->addRow("Refresh (ms):", refreshSpin);
        form->addRow("Overlay X:", posXSpin);
        form->addRow("Overlay Y:", posYSpin);

        main->addLayout(form);

        // Alarm editor (simple)
        QGroupBox* alarmBox = new QGroupBox("Alarms (one per line: coin,currency,threshold)");
        QVBoxLayout* alarmLayout = new QVBoxLayout(alarmBox);
        alarmText = new QPlainTextEdit();
        alarmText->setPlainText(overlay->alarmLines().join("\n"));
        alarmLayout->addWidget(alarmText);
        main->addWidget(alarmBox);

        // Chart area
        QGroupBox* chartBox = new QGroupBox("Historic price chart (first coin/currency)");
        QVBoxLayout* chartLayout = new QVBoxLayout(chartBox);
        chart = new MiniChart();
        chartLayout->addWidget(chart);
        main->addWidget(chartBox);

        // Buttons
        QHBoxLayout* buttons = new QHBoxLayout();
        QPushButton* applyBtn = new QPushButton("Apply");
        QPushButton* closeBtn = new QPushButton("Close");
        QPushButton* refreshChartBtn = new QPushButton("Load Chart");
        buttons->addWidget(refreshChartBtn);
        buttons->addStretch(1);
        buttons->addWidget(applyBtn);
        buttons->addWidget(closeBtn);
        main->addLayout(buttons);

        connect(closeBtn, &QPushButton::clicked, this, &ConfigDialog::hide);
        connect(applyBtn, &QPushButton::clicked, this, &ConfigDialog::apply);
        connect(refreshChartBtn, &QPushButton::clicked, this, &ConfigDialog::loadChart);

        connect(overlay, &PriceOverlay::chartDataReady, this, &ConfigDialog::onChartData);
    }

    void apply() {
        QStringList coins = coinEdit->text().split(',', QString::SkipEmptyParts);
        for (QString &s : coins) s = s.trimmed().toLower();
        if (coins.isEmpty()) coins =  QStringList() << "dogecoin";
        overlay->setCoins(coins);

        QStringList vs = vsEdit->text().split(',', QString::SkipEmptyParts);
        for (QString &s : vs) s = s.trimmed().toLower();
        if (vs.isEmpty()) vs = QStringList() << "usd";
        overlay->setVsCurrencies(vs);

        overlay->setRefreshInterval(refreshSpin->value());
        overlay->move(posXSpin->value(), posYSpin->value());

        // alarms
        QStringList alarmLines = alarmText->toPlainText().split('\n', QString::SkipEmptyParts);
        overlay->setAlarmLines(alarmLines);

        // save settings
        saveSettings();
    }

    void loadSettings() {
        QSettings s("Demo", "CryptoOverlay");
        coinEdit->setText(s.value("coins", "dogecoin").toString());
        vsEdit->setText(s.value("vs", "usd").toString());
        refreshSpin->setValue(s.value("refresh", 30000).toInt());
        posXSpin->setValue(s.value("posx", overlay->x()).toInt());
        posYSpin->setValue(s.value("posy", overlay->y()).toInt());
        alarmText->setPlainText(s.value("alarms", "").toString());
    }

    void saveSettings() {
        QSettings s("Demo", "CryptoOverlay");
        s.setValue("coins", coinEdit->text());
        s.setValue("vs", vsEdit->text());
        s.setValue("refresh", refreshSpin->value());
        s.setValue("posx", posXSpin->value());
        s.setValue("posy", posYSpin->value());
        s.setValue("alarms", alarmText->toPlainText());
    }

public slots:
    void loadChart() {
        overlay->requestChart(7);
    }
    void onChartData(const QVector<QPair<qint64,double>>& d) {
        chart->setData(d);
    }

private:
    PriceOverlay* overlay;
    QNetworkAccessManager* manager;
    QLineEdit* coinEdit;
    QLineEdit* vsEdit;
    QSpinBox* refreshSpin;
    QSpinBox* posXSpin;
    QSpinBox* posYSpin;
    QPlainTextEdit* alarmText;
    MiniChart* chart;
};

// Helper: make sure single instance gets settings loaded at start
int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);

    QNetworkAccessManager manager;

    PriceOverlay overlay(&manager);
    overlay.resize(300, 120);

    // Load settings
    QSettings s("Demo", "CryptoOverlay");
    QString coins = s.value("coins", "dogecoin").toString();
    QString vs = s.value("vs", "usd").toString();
    int refresh = s.value("refresh", 1990000).toInt();
    int px = s.value("posx", 20).toInt();
    int py = s.value("posy", 300).toInt();
    QString alarms = s.value("alarms", "").toString();

    overlay.setCoins(coins.split(',', QString::SkipEmptyParts));
    overlay.setVsCurrencies(vs.split(',', QString::SkipEmptyParts));
    overlay.setRefreshInterval(refresh);
    overlay.move(px, py);
    if (!alarms.isEmpty()) overlay.setAlarmLines(alarms.split('\n', QString::SkipEmptyParts));

    // Tray icon
    QSystemTrayIcon tray(QIcon(":/icon.png"));   // preferred

//    QSystemTrayIcon tray(QIcon()); // empty icon; you should provide QIcon(":/icon.png")
    QMenu trayMenu;
    QAction actShow("Show Overlay");
    QAction actHide("Hide Overlay");
    QAction actSettings("Settings");
    QAction actQuit("Quit");
    trayMenu.addAction(&actShow);
    trayMenu.addAction(&actHide);
    trayMenu.addSeparator();
    trayMenu.addAction(&actSettings);
    trayMenu.addAction(&actQuit);
    QObject::connect(&actShow, &QAction::triggered, [&](){ overlay.show(); });
    QObject::connect(&actHide, &QAction::triggered, [&](){ overlay.hide(); });
    ConfigDialog cfg(&overlay, &manager);
    cfg.loadSettings();
    QObject::connect(&actSettings, &QAction::triggered, [&](){ cfg.show(); });
    QObject::connect(&actQuit, &QAction::triggered, &a, &QApplication::quit);

    tray.setContextMenu(&trayMenu);
    tray.show();

    // alarm handling: show tray message + beep
    QObject::connect(&overlay, &PriceOverlay::alarmTriggered, [&](const QString& msg){
        tray.showMessage("Price Alarm", msg, QSystemTrayIcon::Information, 7000);
        QApplication::beep();
    });

    // chart data -> config dialog
    QObject::connect(&overlay, &PriceOverlay::chartDataReady, &cfg, &ConfigDialog::onChartData);

    overlay.show();

    return a.exec();
}

#include "main.moc"
