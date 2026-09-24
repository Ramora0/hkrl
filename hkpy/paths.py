"""Where things live on this machine: the repo, the sim DLL, the oracle game install and its data folder.

Every path has one default, overridable by an environment variable:

    HKRL_GAME   the oracle install (tools/make_oracle_install.ps1), default <repo>/game
    HKSIM_DLL   the sim, default <repo>/sim/build/hksim.dll

The oracle install writes its saves, settings and mod logs under
%USERPROFILE%/AppData/LocalLow/<company>/Hollow Knight, where <company> is the first line of its
oracle_Data/app.info. make_oracle_install.ps1 renames the company to "hkrl oracle", so the install never
touches the real game's saves; an older install still says "Team Cherry" and shares them.
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = os.environ.get("HKRL_GAME") or os.path.join(ROOT, "game")
SIM_DLL = os.environ.get("HKSIM_DLL") or os.path.join(ROOT, "sim", "build", "hksim.dll")


def managed(game=None):
    """The install's Managed/ (game assemblies, Mods/)."""
    return os.path.join(game or GAME, "oracle_Data", "Managed")


def company(game=None):
    """The company name the install's data folder is keyed on (first line of app.info)."""
    try:
        with open(os.path.join(game or GAME, "oracle_Data", "app.info"), encoding="utf-8") as f:
            return f.readline().strip() or "Team Cherry"
    except OSError:
        return "Team Cherry"


def data_dir(game=None):
    """Application.persistentDataPath of the install: its saves, settings and HKOracle_<exe>.log files."""
    return os.path.join(os.environ["USERPROFILE"], "AppData", "LocalLow", company(game), "Hollow Knight")
