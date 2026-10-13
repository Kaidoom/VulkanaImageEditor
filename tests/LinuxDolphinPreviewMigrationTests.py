"""Exercise the installed KDE migration using disposable XDG profiles only."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
source = Path(sys.argv[1]) if len(sys.argv) > 1 else root / 'res/linux'
update_name = 'vulkana-dolphin-previews.upd'
update_id = 'enable-vulkana-previews-v1'
updater = shutil.which('kconf_update') or next((str(p) for p in (
    Path('/usr/libexec/kf6/kconf_update'),
    Path('/usr/lib/x86_64-linux-gnu/libexec/kf6/kconf_update'),
    Path('/usr/lib/aarch64-linux-gnu/libexec/kf6/kconf_update'),
    Path('/usr/lib/kf6/kconf_update'),
) if p.is_file()), None)
if not updater or not all(shutil.which(p) for p in ('kreadconfig6', 'kwriteconfig6')):
    print('KDE 6 configuration tools unavailable; migration test skipped.')
    sys.exit(77)


def run(args, env):
    result = subprocess.run(list(map(str, args)), env=env, capture_output=True, text=True, timeout=20)
    assert result.returncode == 0, (args, result.returncode, result.stdout, result.stderr)
    return result.stdout.strip()


def read(env, file='dolphinrc', group='PreviewSettings', key='Plugins'):
    return run(['kreadconfig6', '--file', file, '--group', group, '--key', key,
                '--default', 'MISSING'], env)


def write(env, value, file='dolphinrc'):
    run(['kwriteconfig6', '--file', file, '--group', 'PreviewSettings', '--key', 'Plugins', '--', value], env)


with tempfile.TemporaryDirectory(prefix='vulkana-kde-previews-') as directory:
    work = Path(directory)

    def profile(name):
        path = work / name
        for part in ('config', 'data/kconf_update', 'cache', 'state', 'system-config', 'system-data'):
            (path / part).mkdir(parents=True, exist_ok=True)
        for file in (update_name, 'vulkana-dolphin-previews.sh'):
            shutil.copyfile(source / file, path / 'data/kconf_update' / file)
        env = dict(os.environ, QT_QPA_PLATFORM='offscreen',
                   XDG_CONFIG_HOME=str(path / 'config'), XDG_CONFIG_DIRS=str(path / 'system-config'),
                   XDG_DATA_HOME=str(path / 'data'), XDG_DATA_DIRS=str(path / 'system-data'),
                   XDG_CACHE_HOME=str(path / 'cache'), XDG_STATE_HOME=str(path / 'state'))
        # Never notify the real desktop session from kwriteconfig --notify.
        env['DBUS_SESSION_BUS_ADDRESS'] = 'unix:path=' + str(path / 'no-session-bus')
        return path, env

    def migrate(path, env):
        # No --check/--testmode: those intentionally bypass one-time semantics.
        run([updater, path / 'data/kconf_update' / update_name], env)
        assert read(env, 'kconf_updaterc', update_name, 'done') == update_id

    path, env = profile('existing')
    original = 'jpegthumbnail,kraorathumbnail,unavailable-provider,vulkana-old,custom\\,provider'
    write(env, original)
    with (path / 'config/dolphinrc').open('a') as file:
        file.write('\n[General]\nShowFullPath=true\nName=Documents Ω\n')
    migrate(path, env)
    assert read(env) == original + ',vulkana'
    assert read(env, group='General', key='Name') == 'Documents Ω'
    assert read(env, group='General', key='ShowFullPath') == 'true'
    assert read(env, group='General', key='ShowPreviews') == 'MISSING'
    migrate(path, env)
    assert read(env) == original + ',vulkana'  # No duplicate on repeated runs.
    write(env, original)  # A later deliberate opt-out.
    os.utime(path / 'data/kconf_update' / update_name, (2000000000, 2000000000))
    migrate(path, env)
    assert read(env) == original  # Upgrade/reinstall must not turn it back on.

    path, env = profile('fresh')
    migrate(path, env)
    assert read(env) == 'MISSING'
    assert not (path / 'config/dolphinrc').exists()  # Do not narrow KDE's defaults.

    for name, value in (('disabled-all', ''), ('already-enabled', 'vulkana,jpegthumbnail')):
        path, env = profile(name)
        write(env, value)
        before = (path / 'config/dolphinrc').read_bytes()
        migrate(path, env)
        assert (path / 'config/dolphinrc').read_bytes() == before

    path, env = profile('inherited-global')
    write(env, 'jpegthumbnail,kraorathumbnail', file='kdeglobals')
    global_before = (path / 'config/kdeglobals').read_bytes()
    migrate(path, env)
    assert read(env) == 'jpegthumbnail,kraorathumbnail,vulkana'
    assert (path / 'config/kdeglobals').read_bytes() == global_before

    path, env = profile('immutable')
    (path / 'system-config/dolphinrc').write_text('[PreviewSettings]\nPlugins[$i]=jpegthumbnail\n')
    migrate(path, env)
    assert read(env) == 'jpegthumbnail'
    assert not (path / 'config/dolphinrc').exists()

    # Discovery is how KDE runs newly installed updates, without explicit paths.
    path, env = profile('automatic-discovery')
    write(env, 'jpegthumbnail')
    run([updater], env)
    assert read(env) == 'jpegthumbnail,vulkana'
    assert read(env, 'kconf_updaterc', update_name, 'done') == update_id

print('KDE preview defaults: saved lists, fresh profiles, inheritance, immutable settings and later opt-outs passed.')
