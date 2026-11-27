# Builds shots/fake: stand-in emulator folders for testing the first-start
# assistant without touching a real emulator. Nothing in it is real: the
# programs are empty files and the "keys" are zeros.
#   Ryujinx/            a portable Ryujinx (data in Ryujinx/portable) with one profile and one game folder
#   yuzu/build/         a yuzu with its data beside it (yuzu/build/user)
#   games/              one dummy game file
#   pick/               files to hand to the pickers: prod.keys, title.keys, wrong.keys, notanemu.exe
#   appdata/            an empty stand-in for %APPDATA% (NSWHOME_EMUDATA)
import base64, json, os, shutil

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
F = os.path.join(root, 'shots', 'fake')
shutil.rmtree(F, ignore_errors=True)
for d in ['Ryujinx/portable/system', 'games', 'yuzu/build/user/keys', 'appdata', 'pick']:
    os.makedirs(os.path.join(F, d))
for f in ['Ryujinx/Ryujinx.exe', 'yuzu/build/yuzu.exe', 'pick/notanemu.exe']:
    open(os.path.join(F, f), 'wb').close()
open(os.path.join(F, 'games', 'Juego de prueba.nsp'), 'wb').write(b'\0' * 4096)

picture = None
avatars = os.path.join(root, 'data', 'avatars')
for name in sorted(os.listdir(avatars), reverse=True):
    picture = base64.b64encode(open(os.path.join(avatars, name), 'rb').read()).decode()
    break
uid = '0123456789abcdef0123456789abcdef'
json.dump({'profiles': [{'user_id': uid, 'name': 'Portátil', 'account_state': 'Open', 'online_play_state': 'Closed',
                         'last_modified_timestamp': 1700000000, 'image': picture}], 'last_opened': uid},
          open(os.path.join(F, 'Ryujinx/portable/system/Profiles.json'), 'w', encoding='utf-8'), ensure_ascii=False)
json.dump({'game_dirs': [os.path.abspath(os.path.join(F, 'games'))], 'show_console': False},
          open(os.path.join(F, 'Ryujinx/portable/Config.json'), 'w'))
open(os.path.join(F, 'pick/prod.keys'), 'w').write('header_key = ' + '00' * 32 + '\n')
open(os.path.join(F, 'pick/title.keys'), 'w').write('0' * 32 + ' = ' + '0' * 32 + '\n')
open(os.path.join(F, 'pick/wrong.keys'), 'w').write('nothing useful\n')
print('ok')
