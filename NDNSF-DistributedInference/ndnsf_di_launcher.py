"""Start the installed management CLI without legacy user-site packages."""
import os
import sys


def main():
    # Replace this process before importing DI. Keep the selected interpreter
    # and arguments, including inside a container or virtual environment.
    if not sys.flags.no_user_site:
        os.execv(sys.executable, [sys.executable, '-s', sys.argv[0], *sys.argv[1:]])
    from ndnsf_distributed_inference.ops.cli import main as run
    return run()
