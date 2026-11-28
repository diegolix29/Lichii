// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QFile>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include "lichii/nup_dialogs.h"

namespace {

QByteArray ReadRecord(const QString& path, int package, quint16 type) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || !file.seek(0x80 + 0x30 + package * 0x40 + 0x20)) {
        return {};
    }
    const auto extent = file.read(12);
    if (extent.size() != 12 || !file.seek(qFromLittleEndian<quint64>(extent.constData()))) {
        return {};
    }
    const auto block = file.read(qFromLittleEndian<quint32>(extent.constData() + 8));
    for (qsizetype pos = 0; pos + 8 <= block.size();) {
        const auto* header = block.constData() + pos;
        const qsizetype size = qFromLittleEndian<quint32>(header + 4);
        if (size > block.size() - pos - 8) {
            return {};
        }
        if (qFromLittleEndian<quint16>(header) == type) {
            return block.mid(pos, 8 + size);
        }
        pos += 8 + size;
    }
    return {};
}

class NupDialogTest : public QObject {
    Q_OBJECT

private slots:
    void renamePreservesBuildIds();
    void publisherEdits_data();
    void publisherEdits();
};

void NupDialogTest::renamePreservesBuildIds() {
    const auto fixture = QFINDTESTDATA("shifted_base_update.nup");
    QVERIFY(!fixture.isEmpty());
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const auto path = temp.filePath(QStringLiteral("edited.nup"));
    QVERIFY(QFile::copy(fixture, path));
    const auto archive = std::filesystem::path(path.toStdU16String());
    const auto base_id = ReadRecord(fixture, 0, 0x0006);
    const auto update_id = ReadRecord(fixture, 1, 0x0006);
    QCOMPARE(base_id.size(), 40);
    QCOMPARE(update_id.size(), 40);
    QVERIFY(base_id != update_id);

    NupEditDialog dialog(archive);
    auto* name = dialog.findChild<QLineEdit*>(QStringLiteral("nupEditName"));
    QVERIFY(name);
    QTest::keyClicks(name, QStringLiteral("Edited title"));
    const auto edit = dialog.Edit();
    QVERIFY(edit.title_name == std::string("Edited title"));
    std::string error;
    QVERIFY2(FileSys::Nup::EditNupMetadata(archive, edit, &error) ==
                 FileSys::Nup::EditResult::Success,
             error.c_str());
    QCOMPARE(ReadRecord(path, 0, 0x0006), base_id);
    QCOMPARE(ReadRecord(path, 1, 0x0006), update_id);
}

void NupDialogTest::publisherEdits_data() {
    QTest::addColumn<bool>("shifted");
    QTest::addColumn<bool>("clear");

    QTest::newRow("shifted-replace") << true << false;
    QTest::newRow("shifted-clear") << true << true;
    QTest::newRow("spec-replace") << false << false;
    QTest::newRow("spec-clear") << false << true;
}

void NupDialogTest::publisherEdits() {
    QFETCH(bool, shifted);
    QFETCH(bool, clear);
    const auto fixture =
        QFINDTESTDATA(shifted ? "shifted_base_update.nup" : "spec_base_update.nup");
    QVERIFY(!fixture.isEmpty());
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const auto path = temp.filePath(QStringLiteral("edited.nup"));
    QVERIFY(QFile::copy(fixture, path));
    const auto archive = std::filesystem::path(path.toStdU16String());
    QCOMPARE(ReadRecord(fixture, 0, 0x0006).size(), 40);

    NupEditDialog dialog(archive);
    auto* publisher = dialog.findChild<QLineEdit*>(QStringLiteral("nupEditPublisher"));
    QVERIFY(publisher);
    publisher->selectAll();
    const auto text = clear ? QString{} : QStringLiteral("Edited publisher");
    if (clear) {
        QTest::keyClick(publisher, Qt::Key_Backspace);
    } else {
        QTest::keyClicks(publisher, text);
    }
    const auto edit = dialog.Edit();
    QVERIFY(edit.publisher == text.toStdString());
    std::string error;
    const auto result = FileSys::Nup::EditNupMetadata(archive, edit, &error);
    if (shifted) {
        QFile edited(path);
        QFile original(fixture);
        QVERIFY(edited.open(QIODevice::ReadOnly));
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(edited.readAll(), original.readAll());
        QCOMPARE(result, FileSys::Nup::EditResult::InvalidSource);
    } else {
        QVERIFY2(result == FileSys::Nup::EditResult::Success, error.c_str());
        QCOMPARE(ReadRecord(path, 0, 0x0006).mid(8), text.toUtf8());
        QCOMPARE(ReadRecord(path, 0, 0x0007), ReadRecord(fixture, 0, 0x0007));
        QCOMPARE(ReadRecord(path, 1, 0x0007), ReadRecord(fixture, 1, 0x0007));
    }
}

} // namespace

QTEST_MAIN(NupDialogTest)
#include "nup_dialog.moc"

#if !defined(QT_STATICPLUGIN) || defined(__APPLE__)
#define VMA_IMPLEMENTATION
#include "video_core/vulkan_common/vma.h"
#endif
