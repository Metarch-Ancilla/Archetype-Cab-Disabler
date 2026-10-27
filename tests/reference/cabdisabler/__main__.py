import sys
if len(sys.argv) > 1:
    from .cli import main
    raise SystemExit(main())
else:
    raise SystemExit('Open the native Windows or macOS frontend. For CLI help: python -m cabdisabler --help')
