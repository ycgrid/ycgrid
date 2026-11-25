YCGrid
======

A real-time spreadsheet that works the way Excel users expect, driven by Python, for trading and research:
live tables fed from DataFrames, fast charts of millions of points, record and replay, trading screens with
safety controls, Excel's analysis tools, and an MCP server for AI agents.

Usage
-----
1. Double-click ycgrid.exe to run it; no installation needed.
2. examples\ holds complete research and trading screens, each a project folder: open its workbook
   (examples\backtest_report\backtest_report.ycg, ...) and it builds the screen: a backtest report, a volatility
   surface, factor research, pairs trading, a portfolio optimizer, Monte Carlo VaR, an option risk dashboard, an
   option payoff builder, a click-to-trade ladder, live quotes with record and replay and more (examples\README.md
   lists them all). They use simulated data; their shared code is in examples\lib.
3. open_demo.cmd opens the demo project (examples\demo\demo.ycg with its Python code in examples\demo\main.py).
4. examples\txo_market_maker is a complete trading screen project: open txo_mm.ycg. Orders go to a paper
   exchange, never to a real market.
5. Developer > Immediate opens a Python prompt; Developer > VS Code (Alt+F11) sets up a project for coding
   agents (AGENTS.md, the YCGrid skill and the MCP server).
6. To open .ycg files with YCGrid by double-clicking them: double-click install.cmd.
   - It copies YCGrid to %LOCALAPPDATA%\Programs\YCGrid, sets the .ycg file association in the current user's
     registry and puts YCGrid on the desktop and in the Start menu; no administrator needed.
   - Without the desktop shortcut: run install.cmd /nodesktop in a command prompt.
   - To use this folder directly instead of copying: run install.cmd /portable in a command prompt.
7. To remove it: double-click uninstall.cmd. Your .ycg files and Python code are not deleted.

Python
------
Scripts outside YCGrid connect to it:
    runtime\python\python.exe -c "import sys; sys.path.insert(0, 'python'); import yc; yc.connect(); ..."
or run python -m yc open examples\backtest_report\backtest_report.ycg with this folder's Python. A wheel for your own Python is built by
scripts\build_wheel.cmd in the source tree (pip install ycgrid-*.whl).

Folder contents
---------------
ycgrid.exe          the program (the icon font embedded, no other DLLs needed)
python\yc\         the Python package (Excel object model, yc.data, yc.plot, yc.replay, yc.trading...)
runtime\           (when packed with Python) portable Python 3.12; the Python on your system is not affected
skill\, mcp\       the YCGrid skill and MCP tool definitions for AI agents
examples\          the example projects: the demo, the trading screen and the research and trading screens
install.cmd        one-step install (file association, desktop and Start menu shortcuts)
uninstall.cmd      one-step removal
licenses\          YCGrid's license, NOTICE.txt, THIRD-PARTY-NOTICES.md and the third-party licenses

Trademarks
----------
Microsoft, Excel and Office are trademarks of the Microsoft group of companies. YCGrid is an independent
project; it is not affiliated with, endorsed by or sponsored by Microsoft.
