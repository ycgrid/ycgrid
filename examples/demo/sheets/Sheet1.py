# SPDX-License-Identifier: Apache-2.0
"""Sheet module of Quotes (CodeName Sheet1): control events and Worksheet_* events."""
from yc.excel import *

Me = SheetModule(__name__)
Range, Cells = Me.Range, Me.Cells


def CommandButton1_Click():
    import main

    main.randomize(scale=0.03)
    Application.StatusBar = "Quotes randomized"


def ComboBox1_Change():
    # filter the Change column (column 4) by the combo box choice
    choice = Me.ComboBox1.Value
    if choice == "Up":
        Range("A1").AutoFilter(4, ">0")
    elif choice == "Down":
        Range("A1").AutoFilter(4, "<0")
    else:
        Range("A1").AutoFilter(4)


def Worksheet_Change(Target):
    # when the Side column is "Buy", make the name of that row bold
    if Target.Column == 7:
        for cell in Target:
            Cells(cell.Row, 2).Font.Bold = cell.Value == "Buy"
