#include "BBSDirectoryDialog.h"
#include "BBSStationEditor.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QGroupBox>
#include <QMetaObject>

// Sorts numerically by UserRole int value; displays UserRole+20 string
class NumericSortItem : public QTableWidgetItem {
public:
    bool operator<(const QTableWidgetItem &other) const override {
        return data(Qt::UserRole).toInt() < other.data(Qt::UserRole).toInt();
    }
};

BBSDirectoryDialog::BBSDirectoryDialog(BBSDirectory *dir,
                                       const QString &currentBand,
                                       int currentBandwidth,
                                       QWidget *parent)
    : QDialog(parent)
    , m_dir(dir)
    , m_bandFilter(currentBand)
    , m_bwFilter(currentBandwidth)
    , m_userLat(0.0)
    , m_userLon(0.0)
    , m_userGridValid(false)
{
    // Load user grid for distance calculation
    QString ug = BBSDirectory::userGrid();
    if (!ug.isEmpty())
        m_userGridValid = BBSDirectory::gridToLatLon(ug, m_userLat, m_userLon);
    setWindowTitle("BBS Directory");
    setMinimumWidth(980);
    setMinimumHeight(500);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    // ---- Top bar: UTC clock, band filter status ----
    QHBoxLayout *topBar = new QHBoxLayout();
    m_clockLabel = new QLabel();
    m_clockLabel->setStyleSheet("font-weight: bold;");
    topBar->addWidget(m_clockLabel);

    m_bandLabel = new QLabel();
    m_bandLabel->setStyleSheet("color: #444;");
    topBar->addWidget(m_bandLabel);

    topBar->addStretch();

    if (m_bwFilter > 0) {
        QLabel *bwLabel = new QLabel(QString("VARA BW: %1 Hz").arg(m_bwFilter));
        bwLabel->setStyleSheet("color: #444;");
        topBar->addWidget(bwLabel);
    }

    m_showAll = new QCheckBox("Show All");
    topBar->addWidget(m_showAll);

    QPushButton *refreshBtn = new QPushButton("Refresh Online");
    topBar->addWidget(refreshBtn);

    mainLayout->addLayout(topBar);

    // ---- Bandwidth / hour warning ----
    m_bwWarning = new QLabel();
    m_bwWarning->setStyleSheet("color: orange;");
    m_bwWarning->setWordWrap(true);
    m_bwWarning->hide();
    mainLayout->addWidget(m_bwWarning);

    // ---- Table ----
    m_table = new QTableWidget(0, 9);
    m_table->setHorizontalHeaderLabels({"Callsign", "Network", "Grid", "Dist (km)", "Prob.", "Band", "Freq (kHz)", "BW", "Hours (UTC)"});
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(7, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setMinimumSectionSize(55);
    m_table->horizontalHeader()->resizeSection(8, 80);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSortingEnabled(true);
    mainLayout->addWidget(m_table);

    // ---- Info label ----
    m_infoLabel = new QLabel();
    m_infoLabel->setStyleSheet("color: #555;");
    mainLayout->addWidget(m_infoLabel);

    // ---- Bottom button row ----
    QHBoxLayout *btnRow = new QHBoxLayout();

    QPushButton *addBtn = new QPushButton("Add");
    m_editBtn   = new QPushButton("Edit");
    m_deleteBtn = new QPushButton("Delete");
    m_editBtn->setEnabled(false);
    m_deleteBtn->setEnabled(false);

    btnRow->addWidget(addBtn);
    btnRow->addWidget(m_editBtn);
    btnRow->addWidget(m_deleteBtn);
    btnRow->addStretch();

    m_selectBtn = new QPushButton("Select & QSY");
    m_selectBtn->setEnabled(false);
    m_selectBtn->setDefault(true);
    QPushButton *cancelBtn = new QPushButton("Cancel");
    btnRow->addWidget(m_selectBtn);
    btnRow->addWidget(cancelBtn);
    mainLayout->addLayout(btnRow);

    // ---- Connections ----
    connect(m_showAll,  &QCheckBox::stateChanged,       this, &BBSDirectoryDialog::onShowAllChanged);
    connect(refreshBtn, &QPushButton::clicked,           this, &BBSDirectoryDialog::onRefresh);
    connect(m_table,    &QTableWidget::itemSelectionChanged, this, &BBSDirectoryDialog::onRowSelected);
    connect(m_table,    &QTableWidget::cellDoubleClicked, this, [this](){ onSelect(); });
    connect(m_selectBtn,&QPushButton::clicked,           this, &BBSDirectoryDialog::onSelect);
    connect(cancelBtn,  &QPushButton::clicked,           this, &QDialog::reject);
    connect(addBtn,     &QPushButton::clicked,           this, &BBSDirectoryDialog::onAddStation);
    connect(m_editBtn,  &QPushButton::clicked,           this, &BBSDirectoryDialog::onEditStation);
    connect(m_deleteBtn,&QPushButton::clicked,           this, &BBSDirectoryDialog::onDeleteStation);

    // ---- HF conditions ----
    connect(m_dir, &BBSDirectory::conditionsReady, this, &BBSDirectoryDialog::onConditionsReady);
    m_dir->fetchHFConditions();

    // ---- Clock timer ----
    m_clockTimer = new QTimer(this);
    connect(m_clockTimer, &QTimer::timeout, this, &BBSDirectoryDialog::onUpdateClock);
    m_clockTimer->start(1000);
    onUpdateClock();

    // ---- Initial population ----
    applyFilters();
}

void BBSDirectoryDialog::onUpdateClock()
{
    QDateTime utc = QDateTime::currentDateTimeUtc();
    m_clockLabel->setText("UTC: " + utc.toString("HH:mm:ss") + "  ");
}

void BBSDirectoryDialog::onShowAllChanged(int /*state*/)
{
    applyFilters();
}

void BBSDirectoryDialog::applyFilters()
{
    int utcHour = QDateTime::currentDateTimeUtc().time().hour();

    QList<BBSStation> stations;
    if (m_showAll->isChecked()) {
        stations = m_dir->stations();
        m_bwWarning->hide();
        m_bandLabel->clear();
    } else {
        stations = m_dir->filtered(m_bandFilter, m_bwFilter, utcHour);

        // Update band label
        if (!m_bandFilter.isEmpty())
            m_bandLabel->setText(QString("Band filter: %1").arg(m_bandFilter));
        else
            m_bandLabel->setText("No band filter (rig not connected)");

        // Warn about hidden stations
        QList<BBSStation> allBand = m_dir->filtered(m_bandFilter, 0, -1);
        int hiddenByBw   = 0;
        int hiddenByHour = 0;
        for (const BBSStation &st : allBand) {
            for (const BBSBandEntry &be : st.bands) {
                if (m_bwFilter > 0 && !be.bandwidth.contains(m_bwFilter))
                    hiddenByBw++;
                else if (!hourMatch_local(be.hours, utcHour))
                    hiddenByHour++;
            }
        }
        // Just show a summary warning if any are hidden
        QStringList warnings;
        if (hiddenByBw > 0)
            warnings << QString("%1 entr%2 hidden: bandwidth mismatch with VARA setting (%3 Hz). Change BW in VARA Setup and reconnect.")
                            .arg(hiddenByBw).arg(hiddenByBw == 1 ? "y" : "ies").arg(m_bwFilter);
        if (hiddenByHour > 0)
            warnings << QString("%1 station entr%2 not shown — outside operating hours at UTC %3h. Check \"Show All\" to see them.")
                            .arg(hiddenByHour).arg(hiddenByHour == 1 ? "y" : "ies").arg(utcHour);
        if (!warnings.isEmpty()) {
            m_bwWarning->setText(warnings.join("\n"));
            m_bwWarning->show();
        } else {
            m_bwWarning->hide();
        }
    }

    populateTable(stations);
}

// Local helper (can't use private member in lambda easily)
bool BBSDirectoryDialog::hourMatch_local(const QString &hours, int utcHour) const
{
    QStringList parts = hours.split('-');
    if (parts.size() != 2) return true;
    int start = parts[0].toInt();
    int end   = parts[1].toInt();
    if (start == end) return true;
    if (start < end) return utcHour >= start && utcHour < end;
    return utcHour >= start || utcHour < end;
}

void BBSDirectoryDialog::populateTable(const QList<BBSStation> &stations)
{
    m_table->setSortingEnabled(false);
    m_table->setRowCount(0);

    int rowCount = 0;
    for (const BBSStation &st : stations) {
        for (const BBSBandEntry &be : st.bands) {
            m_table->insertRow(rowCount);

            auto centered = [](const QString &text) {
                QTableWidgetItem *i = new QTableWidgetItem(text);
                i->setTextAlignment(Qt::AlignCenter);
                return i;
            };

            QString bwStr;
            for (int i = 0; i < be.bandwidth.size(); ++i) {
                if (i) bwStr += "/";
                bwStr += QString::number(be.bandwidth[i]);
            }

            double freqKhz = be.frequency / 1000.0;

            // Distance calculation
            double stLat, stLon;
            QString distStr = "—";
            double distVal = -1.0;
            if (m_userGridValid && !st.grid.isEmpty() &&
                BBSDirectory::gridToLatLon(st.grid, stLat, stLon))
            {
                distVal = BBSDirectory::haversineKm(m_userLat, m_userLon, stLat, stLon);
                distStr = QString::number(qRound(distVal));
            }

            // Col 0: Callsign
            m_table->setItem(rowCount, 0, new QTableWidgetItem(st.callsign));
            m_table->setItem(rowCount, 1, new QTableWidgetItem(st.network));
            m_table->setItem(rowCount, 2, centered(st.grid.toUpper()));

            // Col 3: Distance
            QTableWidgetItem *distItem = new QTableWidgetItem();
            distItem->setData(Qt::DisplayRole, distVal >= 0 ? QVariant(qRound(distVal)) : QVariant("—"));
            distItem->setTextAlignment(Qt::AlignCenter);
            m_table->setItem(rowCount, 3, distItem);

            // Col 4: Probability — placeholder "..." until HamQSL responds
            NumericSortItem *probItem = new NumericSortItem();
            probItem->setText("...");
            probItem->setTextAlignment(Qt::AlignCenter);
            probItem->setData(Qt::UserRole, -1);          // sort key
            probItem->setData(Qt::UserRole + 10, distVal);
            probItem->setData(Qt::UserRole + 11, be.band);
            m_table->setItem(rowCount, 4, probItem);

            // Col 5: Band
            m_table->setItem(rowCount, 5, centered(be.band));

            // Col 6: Freq
            QTableWidgetItem *freqItem = new QTableWidgetItem();
            freqItem->setData(Qt::DisplayRole, freqKhz);
            freqItem->setTextAlignment(Qt::AlignCenter);
            m_table->setItem(rowCount, 6, freqItem);

            m_table->setItem(rowCount, 7, centered(bwStr));
            m_table->setItem(rowCount, 8, centered(be.hours));

            // Store data for retrieval
            m_table->item(rowCount, 0)->setData(Qt::UserRole,     st.callsign);
            m_table->item(rowCount, 6)->setData(Qt::UserRole + 1, (quint64)be.frequency);
            m_table->item(rowCount, 5)->setData(Qt::UserRole + 2, be.band);

            // Tooltip: sysop, location, grid, notes
            QString tip = QString("%1 — %2\nGrid: %3\n%4").arg(st.sysop, st.location, st.grid, st.notes).trimmed();
            for (int c = 0; c < 9; ++c)
                if (m_table->item(rowCount, c))
                    m_table->item(rowCount, c)->setToolTip(tip);

            ++rowCount;
        }
    }

    m_table->setSortingEnabled(true);
    m_table->sortByColumn(3, Qt::AscendingOrder);   // Distance until % is ready
    m_infoLabel->setText(QString("%1 station entr%2").arg(rowCount).arg(rowCount == 1 ? "y" : "ies"));
    m_selectBtn->setEnabled(false);
    m_editBtn->setEnabled(false);
    m_deleteBtn->setEnabled(false);
}

void BBSDirectoryDialog::onRowSelected()
{
    bool sel = !m_table->selectedItems().isEmpty();
    m_selectBtn->setEnabled(sel);
    m_editBtn->setEnabled(sel);
    m_deleteBtn->setEnabled(sel);
}

void BBSDirectoryDialog::onRefresh()
{
    const QString url = "https://raw.githubusercontent.com/LiaisonOS/data/main/bbs-directory.json";
    m_dir->fetchFromUrl(url);

    auto *connLoaded = new QMetaObject::Connection();
    auto *connError  = new QMetaObject::Connection();
    *connLoaded = connect(m_dir, &BBSDirectory::loaded, this, [this, connLoaded, connError](){
        disconnect(*connLoaded); delete connLoaded;
        disconnect(*connError);  delete connError;
        applyFilters();
    });
    *connError = connect(m_dir, &BBSDirectory::fetchError, this, [this, connLoaded, connError](const QString &err){
        disconnect(*connLoaded); delete connLoaded;
        disconnect(*connError);  delete connError;
        QMessageBox::warning(this, "Refresh Failed", "Could not update BBS directory:\n" + err);
    });
}

void BBSDirectoryDialog::onSelect()
{
    int row = m_table->currentRow();
    if (row < 0) return;

    QTableWidgetItem *callItem = m_table->item(row, 0);
    QTableWidgetItem *freqItem = m_table->item(row, 6);
    QTableWidgetItem *bandItem = m_table->item(row, 5);
    if (!callItem || !freqItem || !bandItem) return;

    m_selection.callsign  = callItem->data(Qt::UserRole).toString();
    m_selection.frequency = freqItem->data(Qt::UserRole + 1).toULongLong();
    m_selection.band      = bandItem->data(Qt::UserRole + 2).toString();

    accept();
}

void BBSDirectoryDialog::onAddStation()
{
    BBSStation blank;
    blank.localOnly = true;
    BBSStationEditor editor(blank, this);
    if (editor.exec() != QDialog::Accepted) return;

    BBSStation added = editor.result();
    added.localOnly = true;
    m_dir->addOrUpdateStation(added);
    applyFilters();
}

void BBSDirectoryDialog::onEditStation()
{
    int row = m_table->currentRow();
    if (row < 0) return;

    QString callsign = m_table->item(row, 0)->data(Qt::UserRole).toString();

    // Find the station in the directory
    BBSStation found;
    bool ok = false;
    for (const BBSStation &st : m_dir->stations()) {
        if (st.callsign.toUpper() == callsign.toUpper()) {
            found = st;
            ok = true;
            break;
        }
    }
    if (!ok) return;

    BBSStationEditor editor(found, this);
    if (editor.exec() != QDialog::Accepted) return;

    m_dir->addOrUpdateStation(editor.result());
    applyFilters();
}

void BBSDirectoryDialog::onDeleteStation()
{
    int row = m_table->currentRow();
    if (row < 0) return;

    QString callsign = m_table->item(row, 0)->data(Qt::UserRole).toString();

    if (QMessageBox::question(this, "Delete Station",
            QString("Remove %1 from the directory?").arg(callsign),
            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
        return;

    m_dir->removeStation(callsign);
    applyFilters();
}

void BBSDirectoryDialog::onConditionsReady(HFConditions cond)
{
    m_conditions = cond;
    updateProbColumn();
}

void BBSDirectoryDialog::updateProbColumn()
{
    int utcHour = QDateTime::currentDateTimeUtc().time().hour();
    m_table->setSortingEnabled(false);
    for (int row = 0; row < m_table->rowCount(); ++row)
    {
        QTableWidgetItem *probItem = m_table->item(row, 4);
        if (!probItem) continue;
        double distKm = probItem->data(Qt::UserRole + 10).toDouble();
        QString band  = probItem->data(Qt::UserRole + 11).toString();
        if (distKm < 0) {
            probItem->setText("—");
            probItem->setData(Qt::UserRole, -1);
            continue;
        }
        int prob = BBSDirectory::connectionProbability(band, distKm, utcHour, m_conditions);
        probItem->setData(Qt::DisplayRole, prob);
        probItem->setData(Qt::UserRole, prob);
        if (prob >= 70)
            probItem->setBackground(QColor(0, 180, 0, 80));
        else if (prob >= 40)
            probItem->setBackground(QColor(255, 165, 0, 80));
        else
            probItem->setBackground(QColor(220, 0, 0, 80));
    }
    m_table->setSortingEnabled(true);
    m_table->sortByColumn(4, Qt::DescendingOrder);
}
