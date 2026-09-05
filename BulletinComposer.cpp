/*
 * BulletinComposer.cpp
 *
 * Author : Sylvain Deguire (VA2OPS)
 */

#include "BulletinComposer.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QMessageBox>
#include <QFontDatabase>


BulletinComposer::BulletinComposer(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("LiaisonOS — Compose Bulletin");
    setMinimumSize(720, 560);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    QFormLayout *form = new QFormLayout();

    m_typeCombo = new QComboBox();
    m_typeCombo->addItem("Bulletin (B)", "B");
    m_typeCombo->addItem("Personal (P)", "P");
    form->addRow("Type", m_typeCombo);

    m_toEdit = new QLineEdit();
    m_toEdit->setPlaceholderText("SITREP, EWN, WX, BDN, or callsign");
    form->addRow("To / Category", m_toEdit);

    m_titleEdit = new QLineEdit();
    m_titleEdit->setPlaceholderText("Title");
    form->addRow("Title", m_titleEdit);

    m_targetBbsEdit = new QLineEdit();
    m_targetBbsEdit->setPlaceholderText("e.g. VE2BBS");
    form->addRow("Target BBS", m_targetBbsEdit);

    m_haRouteEdit = new QLineEdit();
    m_haRouteEdit->setPlaceholderText("optional, e.g. VE2BBS.#QC.QC.CAN.NOAM");
    form->addRow("HA route", m_haRouteEdit);

    mainLayout->addLayout(form);

    mainLayout->addWidget(new QLabel("Body — plain text, line breaks preserved"));
    m_bodyEdit = new QPlainTextEdit();
    // Match QtTermTCP's terminal pane: monospace + the same default size.
    m_bodyEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_bodyEdit->setTabChangesFocus(true);
    mainLayout->addWidget(m_bodyEdit, /*stretch=*/1);

    QHBoxLayout *btnBar = new QHBoxLayout();
    btnBar->addStretch();
    m_cancelBtn = new QPushButton("Cancel");
    m_saveBtn = new QPushButton("Save");
    m_saveBtn->setDefault(true);
    btnBar->addWidget(m_cancelBtn);
    btnBar->addWidget(m_saveBtn);
    mainLayout->addLayout(btnBar);

    connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_saveBtn,   &QPushButton::clicked, this, &BulletinComposer::onSave);
}


void BulletinComposer::loadForEdit(const Bulletin &b)
{
    m_isEdit = true;
    m_editing = b;

    int idx = m_typeCombo->findData(b.type);
    if (idx >= 0) m_typeCombo->setCurrentIndex(idx);
    m_toEdit->setText(b.to);
    m_titleEdit->setText(b.title);
    m_targetBbsEdit->setText(b.atBbs);
    m_haRouteEdit->setText(b.haRoute);
    m_bodyEdit->setPlainText(b.body);

    setWindowTitle("LiaisonOS — Edit Bulletin");
}


void BulletinComposer::onSave()
{
    QString title   = m_titleEdit->text().trimmed();
    QString body    = m_bodyEdit->toPlainText();
    QString to      = m_toEdit->text().trimmed();
    QString bbs     = m_targetBbsEdit->text().trimmed().toUpper();

    QStringList missing;
    if (title.isEmpty()) missing << "Title";
    if (to.isEmpty())    missing << "To / Category";
    if (bbs.isEmpty())   missing << "Target BBS";
    if (body.trimmed().isEmpty()) missing << "Body";
    if (!missing.isEmpty()) {
        QMessageBox::warning(this, "Missing fields",
            "Please fill in: " + missing.join(", "));
        return;
    }

    Bulletin b;
    if (m_isEdit) {
        b = m_editing;
    } else {
        b.id      = BulletinStore::generateId(title);
        b.created = QDateTime::currentDateTime();
        b.status  = "pending";
    }
    b.type    = m_typeCombo->currentData().toString();
    b.to      = to;
    b.title   = title;
    b.atBbs   = bbs;
    b.haRoute = m_haRouteEdit->text().trimmed();
    b.body    = body;

    QString err;
    if (!BulletinStore::save(b, &err)) {
        QMessageBox::critical(this, "Save failed", err);
        return;
    }
    m_result = b;
    accept();
}
