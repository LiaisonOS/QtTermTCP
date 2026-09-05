/*
 * BulletinComposer.h
 *
 * Compose / edit a BBS bulletin and save it to the pending/ store.
 *
 * Author : Sylvain Deguire (VA2OPS)
 */

#ifndef BULLETINCOMPOSER_H
#define BULLETINCOMPOSER_H

#include <QDialog>
#include "Bulletin.h"

class QLineEdit;
class QComboBox;
class QPlainTextEdit;
class QPushButton;

class BulletinComposer : public QDialog
{
    Q_OBJECT

public:
    explicit BulletinComposer(QWidget *parent = nullptr);

    void loadForEdit(const Bulletin &b);
    Bulletin result() const { return m_result; }

private slots:
    void onSave();

private:
    QComboBox      *m_typeCombo;
    QLineEdit      *m_toEdit;
    QLineEdit      *m_titleEdit;
    QLineEdit      *m_targetBbsEdit;
    QLineEdit      *m_haRouteEdit;
    QPlainTextEdit *m_bodyEdit;
    QPushButton    *m_saveBtn;
    QPushButton    *m_cancelBtn;

    Bulletin m_editing;
    Bulletin m_result;
    bool m_isEdit = false;
};

#endif
