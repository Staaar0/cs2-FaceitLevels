import sys
try:
  from ambuild2 import run
except ImportError:
  sys.stderr.write('AMBuild 2.2 or newer is required: https://github.com/alliedmodders/ambuild\n')
  sys.exit(1)

parser = run.BuildParser(sourcePath=sys.path[0], api='2.2')
parser.options.add_argument('--hl2sdk-root', type=str, dest='hl2sdk_root', default=None,
                            help='Folder that contains hl2sdk-cs2')
parser.options.add_argument('--hl2sdk-manifests', type=str, dest='hl2sdk_manifests', default=None,
                            help='hl2sdk-manifests source tree')
parser.options.add_argument('--mms_path', type=str, dest='mms_path', default=None,
                            help='Metamod:Source 2.0 source tree')
parser.options.add_argument('--enable-debug', action='store_const', const='1', dest='debug',
                            help='Build with debugging symbols')
parser.options.add_argument('--enable-optimize', action='store_const', const='1', dest='opt',
                            help='Build with optimization (default)')
parser.Configure()
