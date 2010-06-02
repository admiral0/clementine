/* This file is part of Clementine.

   Clementine is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Clementine is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Clementine.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef ERRORDIALOG_H
#define ERRORDIALOG_H

#include <QDialog>

class Ui_ErrorDialog;

class ErrorDialog : public QDialog {
  Q_OBJECT

public:
  ErrorDialog(QWidget* parent = 0);
  ~ErrorDialog();

public slots:
  void ShowMessage(const QString& message);

protected:
  void hideEvent(QHideEvent *);

private:
  void UpdateContent();

  Ui_ErrorDialog* ui_;

  QStringList current_messages_;
};

#endif // ERRORDIALOG_H