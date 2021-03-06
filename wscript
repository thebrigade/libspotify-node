from shutil import copy2 as copy
import Options
import Utils
import os
import os.path as path
import platform

srcdir = "."
blddir = 'build'
VERSION = '0.0.1'

PLATFORM_IS_DARWIN = platform.platform().find('Darwin') == 0

# OS X dylib linker fix
from TaskGen import feature, after
@feature('cshlib')
@after('apply_obj_vars', 'apply_link')
def kill_flag(self):
  fl = self.link_task.env.LINKFLAGS
  if '-bundle' in fl and '-dynamiclib' in fl:
     fl.remove('-bundle')
     self.link_task.env.LINKFLAGS = fl

def set_options(opts):
  opts.tool_options('compiler_cxx')
  opts.add_option('--debug', action='store_true', default=False,
                  help='build debug version')

def configure(conf):
  # todo: add --debug flag so we can set NDEBUG conditionally, omitting asserts.
  conf.check_tool('compiler_cxx')
  conf.check_tool('node_addon')
  conf.env.append_value("LIB_spotify", "spotify")

def lint(ctx):
  Utils.exec_command('python cpplint.py --verbose=0 --filter='+
    '-legal/copyright,' +     # in the future
    '-build/header_guard,' +  # not interesting
    '-build/include,' +       # lint is run from outside src
    '-build/namespaces,' +    # we are not building a C++ API
    '-whitespace/comments,' +
    ' src/*.cc')

def build(ctx):
  ctx.add_pre_fun(lint)
  task = ctx.new_task_gen('cxx', 'shlib', 'node_addon')
  task.target = 'binding'
  task.source = ctx.path.ant_glob('src/*.cc')
  task.uselib = 'spotify'

def test(ctx):
  status = Utils.exec_command('node test/all.js')
  if status != 0:
    raise Utils.WafError('tests failed')

def shutdown():
  # HACK to get binding.node out of build directory
  if 'clean' in Options.commands:
    if path.exists('spotify/binding.node'): os.unlink('spotify/binding.node')
  elif path.exists('build/default/binding.node'):
      copy('build/default/binding.node', 'spotify/binding.node')

