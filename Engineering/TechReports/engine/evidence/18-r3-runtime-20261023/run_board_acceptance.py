"""Run the registered board checks, reusing only byte-matched compiler inputs."""
import argparse,sys
from pathlib import Path
root=next(p for p in Path(__file__).resolve().parents if (p/'Main/tools/check/validate.py').exists())
sys.path.insert(0,str(root/'Main/tools/check'))
import validate
p=argparse.ArgumentParser();p.add_argument('--reuse-from',required=True);a=p.parse_args()
# The existing synthesis checker verifies every compiler input and deployed bit identity.
# The resulting plan records this exact additional argument; the checks themselves are unchanged.
validate.CHECKS['r3-synthesis']['command']+=['--reuse-from',a.reuse_from]
sys.argv=['validate.py','--checks','r3-legacy-board','r3-board','--profile','acceptance','--run']
validate.main()
