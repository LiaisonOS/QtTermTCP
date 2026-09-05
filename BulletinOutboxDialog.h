/*
 * BulletinOutboxDialog.h
 *
 * Outbox browser for staged BBS bulletins. Displays pending/, sent/ and
 * failed/ entries in a single table view with filter buttons. Operator can
 * compose new bulletins, edit/duplicate/delete pending ones, and view sent
 * receipts.
 *
 * Phase 1: read-only against pending/. No Send-now action — sending is
 * deferred to Phase 2's QtTermTCP --send-bulletin headless mode so there is
 * exactly one production send path.
 *
 * Author : Sylvain Deguire (VA2OPS)
 */

#ifndef BULLETINOUTBOXDIALOG_H
#define BULLETINOUTBOXDIALOG_H

#include <QDialog>
#include <QString>

#include "Bulletin.h"

class QTableWidget;
class QPushButton;
class QButtonGroup;
class QLabel;
class QFileSystemWatcher;
class QTimer;

class BulletinOutboxDialog : public QDialog
{
    Q_OBJECT

public:
    explicit BulletinOutboxDialog(QWidget *parent = nullptr);

private slots:
    void refresh();
    void onCompose();
    void onEdit();
    void onDuplicate();
    void onDelete();
    void onFilterChanged(int id);
    void onStoreChanged();

private:
    QString currentSubDir() const;
    QString selectedId() const;
    void populateTable(const QList<Bulletin> &items);

    QTableWidget   *m_table;
    QPushButton    *m_composeBtn;
    QPushButton    *m_editBtn;
    QPushButton    *m_dupBtn;
    QPushButton    *m_delBtn;
    QPushButton    *m_refreshBtn;
    QButtonGroup   *m_filterGroup;
    QLabel         *m_countLabel;

    QFileSystemWatcher *m_watcher;
    QTimer             *m_coalesce;

    int m_filterId = 0;
};

#endif
