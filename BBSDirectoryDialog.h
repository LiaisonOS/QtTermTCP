#ifndef BBSDIRECTORYDIALOG_H
#define BBSDIRECTORYDIALOG_H

#include <QDialog>
#include <QTableWidget>
#include <QCheckBox>
#include <QLabel>
#include <QTimer>
#include <QPushButton>
#include "BBSDirectory.h"

// Returned when user selects a station entry
struct BBSDirectorySelection {
    QString callsign;
    quint64 frequency;  // Hz, 0 if not applicable
    QString band;
};

class BBSDirectoryDialog : public QDialog
{
    Q_OBJECT

public:
    // currentBand: e.g. "40m", empty = no filter
    // currentBandwidth: 500/2300/2750, 0 = no filter
    explicit BBSDirectoryDialog(BBSDirectory *dir,
                                const QString &currentBand,
                                int currentBandwidth,
                                QWidget *parent = nullptr);

    BBSDirectorySelection selection() const { return m_selection; }

private slots:
    void onShowAllChanged(int state);
    void onRowSelected();
    void onRefresh();
    void onUpdateClock();
    void onSelect();
    void onAddStation();
    void onEditStation();
    void onDeleteStation();
    void onConditionsReady(HFConditions cond);

private:
    void applyFilters();
    void populateTable(const QList<BBSStation> &stations);
    void updateProbColumn();
    bool hourMatch_local(const QString &hours, int utcHour) const;

    BBSDirectory *m_dir;
    QString m_bandFilter;
    int m_bwFilter;

    double m_userLat;
    double m_userLon;
    bool m_userGridValid;
    HFConditions m_conditions;

    QCheckBox *m_showAll;
    QLabel *m_clockLabel;
    QLabel *m_bwWarning;
    QLabel *m_bandLabel;
    QTableWidget *m_table;
    QLabel *m_infoLabel;
    QPushButton *m_selectBtn;
    QPushButton *m_editBtn;
    QPushButton *m_deleteBtn;

    QTimer *m_clockTimer;
    BBSDirectorySelection m_selection;
};

#endif // BBSDIRECTORYDIALOG_H
