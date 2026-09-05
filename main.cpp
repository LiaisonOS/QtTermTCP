#include "QtTermTCP.h"
#include "BulletinSender.h"
#include <QApplication>
#include <QCoreApplication>
#include <QStringList>
#include <qscreen.h>

QApplication * a;

int screenHeight = 0;				// used to control dialog sizes on Android
int screenWidth = 0;

int main(int argc, char *argv[])
{
	// Headless bulletin sender — detect BEFORE constructing QApplication so
	// the process never opens a display connection. Falls through to the
	// normal GUI startup path when --send-bulletin isn't on the command line.
	QStringList rawArgs;
	rawArgs.reserve(argc);
	for (int i = 1; i < argc; ++i) rawArgs << QString::fromLocal8Bit(argv[i]);
	if (rawArgs.contains(QStringLiteral("--send-bulletin"))) {
		QCoreApplication app(argc, argv);
		QCoreApplication::setOrganizationName("LiaisonOS");
		QCoreApplication::setApplicationName("QtTermTCP");
		return BulletinSender::runFromArgs(rawArgs);
	}

	a = new QApplication (argc, argv);

	QSize r = a->screens()[0]->size();

	screenHeight = r.height();
	screenWidth = r.width();

	QtTermTCP w;

//#ifdef ANDROID
//	w.setWindowFlags(Qt::Window | Qt::CustomizeWindowHint); // Qt::FramelessWindowHint);
//#endif
	w.show();
	return a->exec();
}
