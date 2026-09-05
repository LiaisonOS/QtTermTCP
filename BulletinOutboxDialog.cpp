/*
 * BulletinOutboxDialog.cpp
 *
 * Author : Sylvain Deguire (VA2OPS)
 */

#include "BulletinOutboxDialog.h"
#include "BulletinComposer.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTableWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QButtonGroup>
#include <QLabel>
#include <QMessageBox>
#include <QFileSystemWatcher>
#include <QTimer>


BulletinOutboxDialog::BulletinOutboxDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("LiaisonOS — Bulletin Outbox");
    setMinimumSize(880, 520);

    BulletinStore::ensureDirs();

    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    QHBoxLayout *topBar = new QHBoxLayout();

    m_composeBtn = new QPushButton("+ Compose…");
    m_refreshBtn = new QPushButton("Refresh");
    topBar->addWidget(m_composeBtn);
    topBar->addWidget(m_refreshBtn);
    topBar->addSpacing(20);

    topBar->addWidget(new QLabel("View:"));
    QPushButton *pendingBtn = new QPushButton("Pending");
    QPushButton *sentBtn    = new QPushButton("Sent");
    QPushButton *failedBtn  = new QPushButton("Failed");
    for (QPushButton *b : { pendingBtn, sentBtn, failedBtn }) {
        b->setCheckable(true);
    }
    pendingBtn->setChecked(true);

    m_filterGroup = new QButtonGroup(this);
    m_filterGroup->setExclusive(true);
    m_filterGroup->addButton(pendingBtn, 0);
    m_filterGroup->addButton(sentBtn,    1);
    m_filterGroup->addButton(failedBtn,  2);
    topBar->addWidget(pendingBtn);
    topBar->addWidget(sentBtn);
    topBar->addWidget(failedBtn);

    topBar->addStretch();
    m_countLabel = new QLabel("");
    topBar->addWidget(m_countLabel);

    mainLayout->addLayout(topBar);

    m_table = new QTableWidget();
    m_table->setColumnCount(6);
    m_table->setHorizontalHeaderLabels(
        { "Title", "To / Category", "Target BBS", "Type", "Created", "Status" });
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->setColumnWidth(0, 260);
    m_table->setColumnWidth(1, 110);
    m_table->setColumnWidth(2, 110);
    m_table->setColumnWidth(3, 60);
    m_table->setColumnWidth(4, 150);
    m_table->setColumnWidth(5, 120);
    mainLayout->addWidget(m_table, /*stretch=*/1);

    QHBoxLayout *btmBar = new QHBoxLayout();
    m_editBtn = new QPushButton("Edit…");
    m_dupBtn  = new QPushButton("Duplicate");
    m_delBtn  = new QPushButton("Delete");
    btmBar->addStretch();
    btmBar->addWidget(m_editBtn);
    btmBar->addWidget(m_dupBtn);
    btmBar->addWidget(m_delBtn);
    QPushButton *closeBtn = new QPushButton("Close");
    btmBar->addWidget(closeBtn);
    mainLayout->addLayout(btmBar);

    connect(m_composeBtn, &QPushButton::clicked, this, &BulletinOutboxDialog::onCompose);
    connect(m_refreshBtn, &QPushButton::clicked, this, &BulletinOutboxDialog::refresh);
    connect(m_editBtn,    &QPushButton::clicked, this, &BulletinOutboxDialog::onEdit);
    connect(m_dupBtn,     &QPushButton::clicked, this, &BulletinOutboxDialog::onDuplicate);
    connect(m_delBtn,     &QPushButton::clicked, this, &BulletinOutboxDialog::onDelete);
    connect(closeBtn,     &QPushButton::clicked, this, &QDialog::accept);
    connect(m_table, &QTableWidget::doubleClicked,
            this, &BulletinOutboxDialog::onEdit);

    // QButtonGroup::idClicked is the modern signal; idToggled would fire twice
    // (on each side of the exclusive switch), so we deliberately use idClicked.
    connect(m_filterGroup,
            QOverload<int>::of(&QButtonGroup::idClicked),
            this, &BulletinOutboxDialog::onFilterChanged);

    // QFileSystemWatcher fires once per change with no coalescing — burst
    // edits can call refresh() 5+ times in 50 ms. Funnel through a 200 ms
    // single-shot timer so the table redraws once per burst.
    m_watcher = new QFileSystemWatcher(this);
    m_watcher->addPath(BulletinStore::pendingDir());
    m_watcher->addPath(BulletinStore::sentDir());
    m_watcher->addPath(BulletinStore::failedDir());
    m_coalesce = new QTimer(this);
    m_coalesce->setSingleShot(true);
    m_coalesce->setInterval(200);
    connect(m_watcher, &QFileSystemWatcher::directoryChanged,
            this, &BulletinOutboxDialog::onStoreChanged);
    connect(m_coalesce, &QTimer::timeout,
            this, &BulletinOutboxDialog::refresh);

    refresh();
}


QString BulletinOutboxDialog::currentSubDir() const
{
    switch (m_filterId) {
        case 1:  return "sent";
        case 2:  return "failed";
        default: return "pending";
    }
}


QString BulletinOutboxDialog::selectedId() const
{
    int row = m_table->currentRow();
    if (row < 0) return QString();
    QTableWidgetItem *it = m_table->item(row, 0);
    if (!it) return QString();
    return it->data(Qt::UserRole).toString();
}


void BulletinOutboxDialog::populateTable(const QList<Bulletin> &items)
{
    m_table->setRowCount(items.size());
    int row = 0;
    for (const Bulletin &b : items) {
        QTableWidgetItem *titleItem = new QTableWidgetItem(b.title);
        titleItem->setData(Qt::UserRole, b.id);
        QTableWidgetItem *to     = new QTableWidgetItem(b.to);
        QTableWidgetItem *bbs    = new QTableWidgetItem(b.atBbs);
        QTableWidgetItem *type   = new QTableWidgetItem(b.type);
        QTableWidgetItem *when   = new QTableWidgetItem(
            b.created.isValid() ? b.created.toString("yyyy-MM-dd HH:mm")
                                : QString());
        QString statusLabel = b.status;
        if (b.status == "sent" && !b.bid.isEmpty()) {
            statusLabel = "sent · " + b.bid;
            if (!b.messageNumber.isEmpty())
                statusLabel += " (msg " + b.messageNumber + ")";
        }
        QTableWidgetItem *status = new QTableWidgetItem(statusLabel);

        m_table->setItem(row, 0, titleItem);
        m_table->setItem(row, 1, to);
        m_table->setItem(row, 2, bbs);
        m_table->setItem(row, 3, type);
        m_table->setItem(row, 4, when);
        m_table->setItem(row, 5, status);
        ++row;
    }
    m_countLabel->setText(QString("%1 item(s)").arg(items.size()));
}


void BulletinOutboxDialog::refresh()
{
    populateTable(BulletinStore::list(currentSubDir()));
}


void BulletinOutboxDialog::onStoreChanged()
{
    m_coalesce->start();
}


void BulletinOutboxDialog::onFilterChanged(int id)
{
    m_filterId = id;
    bool isPending = (id == 0);
    m_editBtn->setEnabled(isPending);
    m_dupBtn->setEnabled(isPending);
    m_delBtn->setEnabled(isPending || id == 2);
    refresh();
}


void BulletinOutboxDialog::onCompose()
{
    BulletinComposer dlg(this);
    if (dlg.exec() == QDialog::Accepted) {
        m_filterId = 0;
        m_filterGroup->button(0)->setChecked(true);
        refresh();
    }
}


void BulletinOutboxDialog::onEdit()
{
    if (m_filterId != 0) return;
    QString id = selectedId();
    if (id.isEmpty()) return;

    Bulletin b;
    QString err;
    if (!BulletinStore::load(id, "pending", &b, &err)) {
        QMessageBox::critical(this, "Load failed", err);
        return;
    }
    BulletinComposer dlg(this);
    dlg.loadForEdit(b);
    if (dlg.exec() == QDialog::Accepted)
        refresh();
}


void BulletinOutboxDialog::onDuplicate()
{
    QString id = selectedId();
    if (id.isEmpty()) return;
    Bulletin b;
    if (!BulletinStore::load(id, "pending", &b))
        return;
    Bulletin copy = b;
    copy.id = BulletinStore::generateId(b.title);
    copy.created = QDateTime::currentDateTime();
    copy.status = "pending";
    copy.sentAt = QDateTime();
    copy.bid.clear();
    copy.lastError.clear();
    copy.retryCount = 0;
    copy.scheduledMissionId.clear();
    QString err;
    if (!BulletinStore::save(copy, &err)) {
        QMessageBox::critical(this, "Duplicate failed", err);
        return;
    }
    refresh();
}


void BulletinOutboxDialog::onDelete()
{
    QString id = selectedId();
    if (id.isEmpty()) return;
    if (QMessageBox::question(this, "Delete bulletin",
            "Delete this bulletin? This cannot be undone.")
        != QMessageBox::Yes)
        return;
    QString err;
    if (!BulletinStore::remove(id, currentSubDir(), &err)) {
        QMessageBox::critical(this, "Delete failed", err);
        return;
    }
    refresh();
}
