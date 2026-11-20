"""Native integration tests of production services using POSIX files and fake RTOS/GPIO."""
import re
import json
import pathlib
import shutil
import subprocess
import tempfile
import unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]
class ServiceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix='rfid-services-')
        cls.folder = pathlib.Path(cls.work.name)
        cls.fs = cls.folder / 'fs'
        cls.binary = cls.folder / 'services'
        source=(ROOT/'src/esp32-rfid/web.cpp').read_text()
        auth=[]
        for key in ('kApiKeyHeader','kCookieHeader','kSessionCookieName','kAuthTimeoutMs','kMaxSessions'):
            auth.append(next(line for line in source.splitlines() if line.startswith('constexpr ') and key+' ' in line))
        start=source.index('struct SessionEntry');end=source.index('\n};',start)+3
        auth.append(source[start:end]);auth.append('SessionEntry g_sessions[kMaxSessions];')
        for name in ('check_api_key','generate_api_key','clear_sessions','extract_cookie_token','session_valid','check_session','check_auth','send_login_cookie','issue_session'):
            start=re.search(r'(?:bool|void) '+name+r'\(',source).start()
            end=source.index('\n}',start)+2
            auth.append(source[start:end])
        (cls.folder/'auth_policy.inc').write_text('\n'.join(auth))
        source=(ROOT/'src/esp32-rfid/main.cpp').read_text()
        maintenance=[]
        for name in ('app_maintenance','app_set_maintenance','app_prepare_maintenance'):
            start=re.search(r'(?:bool|void) '+name+r'\(',source).start()
            end=source.index('\n}',start)+2 if '\n}' in source[start:] else -1
            if source[start:source.index('\n',start)].endswith('}'):
                end=source.index('\n',start)
            maintenance.append(source[start:end])
        (cls.folder/'maintenance_policy.inc').write_text('\n'.join(maintenance))

        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-pthread',
            '-fsanitize=address,undefined','-fno-omit-frame-pointer', '-include','freertos/task.h',
            '-I',str(cls.folder),'-I',str(ROOT/'tests/host_stubs'),'-I',str(ROOT/'src/esp32-rfid'),
            f'-DRFID_FS_ROOT="{cls.fs}"',str(ROOT/'tests/services_test.cpp'),
            '-Wl,--wrap=fsync','-Wl,--wrap=rename','-o',str(cls.binary)],check=True)
    @classmethod
    def tearDownClass(cls): cls.work.cleanup()
    def run_case(self,name):
        shutil.rmtree(self.fs,ignore_errors=True)
        self.fs.mkdir()
        result=subprocess.run([str(self.binary),name],capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        if name=='users_pages_escape_and_fit':
            pages=[json.loads(line) for line in result.stdout.splitlines()]
            users=[user for page in pages for user in page['users']]
            self.assertEqual([user['uid'] for user in users],[f'{i:08X}' for i in range(130)])

def add_case(name):
    def test(self):self.run_case(name)
    setattr(ServiceTests,'test_'+name,test)
for name in ('decision_relay_precedes_rtc_without_extra_beep','decision_denied_card_keeps_warning_feedback','decision_discards_old_cards_and_requests','maintenance_requires_fresh_idle_ack','wifi_starts_ap_and_recovers_missing_station','restore_bundle_rolls_back_both_files','interrupted_bundle_recovers_on_boot','auth_rejects_bad_config_and_expires_sessions','atomic_storage_preserves_old_data','settings_validate_and_rollback','corrupt_settings_fail_closed',
             'users_restore_replaces_and_validates','users_failed_save_preserves_permissions','users_pages_escape_and_fit',
             'relay_pulse_manual_and_wrap','relay_startup_failure_stays_off',
             'log_rotation_preserves_archive_and_reserve','log_queue_orders_clear_and_append','uart_rejects_malformed_frames'):
    add_case(name)
if __name__=='__main__':unittest.main(verbosity=2)

