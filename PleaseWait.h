#ifndef PLEASEWAIT_H
#define PLEASEWAIT_H

#include "ui_PleaseWait.h"

class PleaseWaitDialog : public QDialog, protected Ui_PleaseWaitDialog
{
	Q_OBJECT
	public:
		PleaseWaitDialog(QWidget* parent = 0, Qt::WindowFlags flags = 0);
		virtual ~PleaseWaitDialog();

		void SetText(QString text) {hide(); label->setText(text); show(); repaint();}


};

#endif // PLEASEWAIT_H
