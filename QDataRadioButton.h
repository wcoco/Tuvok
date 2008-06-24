#pragma once

#ifndef QDATARADIOBUTTON
#define QDATARADIOBUTTON

#include <QtGui/QRadioButton.h>
#include <tools/DICOM/DICOMParser.h>

class QDataRadioButton : public QRadioButton
{
public:
	QDataRadioButton(DICOMStackInfo stack, QWidget *parent=0);
    QDataRadioButton(DICOMStackInfo stack, const QString &text, QWidget *parent=0);
	virtual ~QDataRadioButton() {}

protected:
	unsigned int m_iCurrentImage;
	DICOMStackInfo m_stackInfo;

	virtual void leaveEvent ( QEvent * event );
	virtual void mouseMoveEvent(QMouseEvent *event);

	void SetupInfo();
	void SetStackImage(unsigned int i);

};

#endif // QDATARADIOBUTTON
