import tempfile
import threading
import unittest
import subprocess
import base64
import json
import hashlib
import io
from contextlib import redirect_stdout
from pathlib import Path
from http.server import ThreadingHTTPServer
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.request import Request, urlopen

from steam_host import (
    AppPolicy,
    HostConfiguration,
    HostError,
    SteamHostRequestHandler,
    SteamHostState,
    _certificate_spki_der,
    directory_regular_file_bytes,
    load_tls_context,
    main,
    validate_server_port,
    validate_tls_origin,
)


class SteamHostStateTest(unittest.TestCase):
    def make_state(self, root: Path) -> SteamHostState:
        return SteamHostState(HostConfiguration(
            origin='https://steam-host.test:8443',
            host_id='test-host',
            data_dir=root / 'state',
            apps={
                '480': AppPolicy('480', 'Test game', 'game.exe', '.', False),
            },
            steamcmd=None,
            steam_user=None,
        ))

    def test_spki_der_extraction_requires_no_openssl_binary(self) -> None:
        def tlv(tag: int, value: bytes) -> bytes:
            self.assertLess(len(value), 128)
            return bytes([tag, len(value)]) + value

        spki = tlv(0x30, tlv(0x06, b'\x2a\x03\x04') + tlv(0x03, b'\x00\x01\x02'))
        tbs = tlv(0x30, b''.join([
            tlv(0xA0, tlv(0x02, b'\x02')),  # version
            tlv(0x02, b'\x01'),              # serial number
            tlv(0x30, b''),                   # signature
            tlv(0x30, b''),                   # issuer
            tlv(0x30, b''),                   # validity
            tlv(0x30, b''),                   # subject
            spki,
        ]))
        certificate = tlv(0x30, tbs + tlv(0x30, b'') + tlv(0x03, b'\x00'))
        self.assertEqual(_certificate_spki_der(certificate), spki)
        self.assertEqual(
            base64.b64encode(hashlib.sha256(_certificate_spki_der(certificate)).digest()).decode('ascii'),
            base64.b64encode(hashlib.sha256(spki).digest()).decode('ascii'),
        )

    def test_origin_and_listen_port_must_agree(self) -> None:
        validate_server_port('https://steam-host.test:8443', 8443)
        validate_server_port('https://steam-host.test', 443)
        with self.assertRaises(HostError):
            validate_server_port('https://steam-host.test:8443', 9443)

    def test_tls_origin_validation_requires_matching_subject_alternative_name(self) -> None:
        fixture_dir = Path(__file__).with_name('testdata')
        certificate = fixture_dir / 'tls-origin-cert.pem'
        key = fixture_dir / 'tls-origin-key.pem'
        context, _pin = load_tls_context(certificate, key)
        validate_tls_origin(context, certificate, 'https://steam-host.test:8443')
        with self.assertRaisesRegex(HostError, 'does not match the configured origin'):
            validate_tls_origin(context, certificate, 'https://other-host.test:8443')

    def test_download_reports_staging_bytes_before_verification(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            steamcmd = root / 'steamcmd'
            steamcmd.write_text('#!/bin/sh\n', encoding='utf-8')
            state = SteamHostState(HostConfiguration(
                origin='https://steam-host.test:8443', host_id='test-host',
                data_dir=root / 'state',
                apps={'480': AppPolicy('480', 'Test game', 'game.exe', '.', False)},
                steamcmd=steamcmd, steam_user='host-account',
            ))
            try:
                job_id = 'download-progress-123'
                with state.lock, state.db:
                    state.db.execute("INSERT INTO jobs(job_id, app_id, state) VALUES(?, ?, 'queued')",
                                     (job_id, '480'))

                class FakeSteamCmd:
                    def __init__(self, staging: Path) -> None:
                        self.staging = staging
                        self.poll_count = 0

                    def poll(self) -> int | None:
                        self.poll_count += 1
                        if self.poll_count == 1:
                            (self.staging / 'game.exe').write_bytes(b'game-payload')
                            return None
                        return 0

                    def wait(self) -> int:
                        return 0

                expected_staging = state.staging_dir / '480' / job_id
                with patch('steam_host.subprocess.Popen', return_value=FakeSteamCmd(expected_staging)) as popen, \
                     patch('steam_host.time.sleep'), \
                     patch.object(state, '_set_job_progress', wraps=state._set_job_progress) as progress:
                    state._run_download_job(job_id, '480')
                self.assertEqual(popen.call_args.kwargs['stdin'], subprocess.DEVNULL)
                self.assertEqual(popen.call_args.kwargs['cwd'], str(steamcmd.parent))
                self.assertEqual(
                    popen.call_args.args[0],
                    [str(steamcmd), '+@sSteamCmdForcePlatformType', 'windows',
                     '+force_install_dir', str(expected_staging), '+login', 'host-account',
                     '+app_update', '480', 'validate', '+quit'],
                )
                progress.assert_called_once_with(job_id, len(b'game-payload'))
                complete = state.job(job_id, '480')
                self.assertEqual(complete['state'], 'complete')
                self.assertEqual(complete['transferredBytes'], len(b'game-payload'))
                self.assertEqual(complete['totalBytes'], len(b'game-payload'))
                self.assertEqual(directory_regular_file_bytes(root / 'missing'), 0)
            finally:
                state.close()

    def test_progress_ignores_a_file_replaced_during_directory_scan(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            transient = root / 'transient.bin'
            transient.write_bytes(b'payload')
            with patch.object(Path, 'is_file', return_value=True), \
                 patch.object(Path, 'stat', side_effect=OSError('replaced by SteamCMD')):
                self.assertEqual(directory_regular_file_bytes(root), 0)

    def test_unavailable_session_rejects_download_without_creating_a_job(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            state = self.make_state(Path(temp))
            try:
                with self.assertRaisesRegex(HostError, 'SteamCMD session is not ready'):
                    state.request_download('480')
                with state.lock:
                    jobs = state.db.execute('SELECT job_id FROM jobs').fetchall()
                self.assertEqual(jobs, [])
            finally:
                state.close()

    def test_stalled_steamcmd_job_times_out_and_releases_the_queue(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            steamcmd = root / 'steamcmd'
            steamcmd.write_text('#!/bin/sh\n', encoding='utf-8')
            state = SteamHostState(HostConfiguration(
                origin='https://steam-host.test:8443', host_id='test-host',
                data_dir=root / 'state',
                apps={'480': AppPolicy('480', 'Test game', 'game.exe', '.', False)},
                steamcmd=steamcmd, steam_user='host-account', steamcmd_timeout_seconds=60,
            ))
            try:
                job_id = 'download-timeout-123'
                with state.lock, state.db:
                    state.db.execute("INSERT INTO jobs(job_id, app_id, state) VALUES(?, ?, 'queued')",
                                     (job_id, '480'))

                class StalledSteamCmd:
                    terminated: bool = False

                    def poll(self) -> None:
                        return None

                    def terminate(self) -> None:
                        self.terminated = True

                    def wait(self, timeout: int | None = None) -> int:
                        return 0

                process = StalledSteamCmd()
                with patch('steam_host.subprocess.Popen', return_value=process), \
                     patch('steam_host.time.monotonic', side_effect=[0.0, 61.0]):
                    state._run_download_job(job_id, '480')
                timed_out = state.job(job_id, '480')
                self.assertTrue(process.terminated)
                self.assertEqual(timed_out['state'], 'failed')
                self.assertEqual(timed_out['errorCode'], 'steamcmd_timeout')
            finally:
                state.close()

    def test_noninteractive_session_probe_disables_downloads_after_login_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            steamcmd = root / 'steamcmd'
            steamcmd.write_text('#!/bin/sh\n', encoding='utf-8')
            state = SteamHostState(HostConfiguration(
                origin='https://steam-host.test:8443', host_id='test-host',
                data_dir=root / 'state',
                apps={'480': AppPolicy('480', 'Test game', 'game.exe', '.', False)},
                steamcmd=steamcmd, steam_user='host-account',
            ))
            try:
                with patch('steam_host.subprocess.run',
                           return_value=subprocess.CompletedProcess([], 1)) as run:
                    self.assertEqual(state.check_steamcmd_session(), 'login_required')
                self.assertEqual(
                    run.call_args.args[0],
                    [str(steamcmd), '+login', 'host-account', '+quit'],
                )
                self.assertEqual(run.call_args.kwargs['stdin'], subprocess.DEVNULL)
                self.assertEqual(run.call_args.kwargs['cwd'], str(steamcmd.parent))
                self.assertFalse(state.steamcmd_download_ready())
                self.assertFalse(state.library()[0]['downloadable'])
            finally:
                state.close()

    def test_noninteractive_session_probe_enables_downloads_after_success(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            steamcmd = root / 'steamcmd'
            steamcmd.write_text('#!/bin/sh\n', encoding='utf-8')
            state = SteamHostState(HostConfiguration(
                origin='https://steam-host.test:8443', host_id='test-host',
                data_dir=root / 'state',
                apps={'480': AppPolicy('480', 'Test game', 'game.exe', '.', False)},
                steamcmd=steamcmd, steam_user='host-account',
            ))
            try:
                with patch('steam_host.subprocess.run',
                           return_value=subprocess.CompletedProcess([], 0)) as run:
                    self.assertEqual(state.check_steamcmd_session(), 'ready')
                self.assertEqual(
                    run.call_args.args[0],
                    [str(steamcmd), '+login', 'host-account', '+quit'],
                )
                self.assertEqual(run.call_args.kwargs['timeout'], 120)
                self.assertTrue(state.steamcmd_download_ready())
                self.assertTrue(state.library()[0]['downloadable'])
            finally:
                state.close()

    def test_pairing_requires_host_console_code(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            state = self.make_state(Path(temp))
            try:
                challenge, code = state.begin_pairing('a' * 24, 'WineHua')
                self.assertNotIn('verificationCode', challenge)
                with self.assertRaises(HostError):
                    state.confirm_pairing(challenge['challengeId'], '000000', 'a' * 24)
                pairing = state.confirm_pairing(challenge['challengeId'], code, 'a' * 24)
                self.assertTrue(state.authenticate(pairing['pairingToken']))
            finally:
                state.close()

    def test_revision_manifest_is_immutable_and_content_addressed(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            state = self.make_state(root)
            try:
                source = root / 'download'
                source.mkdir()
                (source / 'game.exe').write_bytes(b'game-payload')
                manifest = state.publish_revision('480', source)
                self.assertEqual(manifest['containerId'], 'steam-480')
                self.assertEqual(manifest['installRelativePath'], 'games/480')
                self.assertFalse(manifest['launchReady'])
                artifact = manifest['artifacts'][0]
                self.assertEqual(artifact['downloadUrl'],
                    'https://steam-host.test:8443/v1/artifacts/' + artifact['sha256'])
                self.assertEqual(state.artifact_path(artifact['sha256']).read_bytes(), b'game-payload')
                revision_root = state.revisions_dir / '480' / manifest['revision']
                self.assertTrue((revision_root.parent / f"{manifest['revision']}.winehua-manifest.json").is_file())
                self.assertFalse((revision_root / 'winehua-manifest.json').exists())
            finally:
                state.close()

    def test_revision_rejects_a_missing_configured_working_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            state = SteamHostState(HostConfiguration(
                origin='https://steam-host.test:8443', host_id='test-host',
                data_dir=root / 'state',
                apps={'480': AppPolicy('480', 'Test game', 'game.exe', 'bin', False)},
                steamcmd=None, steam_user=None,
            ))
            try:
                source = root / 'download'
                source.mkdir()
                (source / 'game.exe').write_bytes(b'game-payload')
                with self.assertRaisesRegex(HostError, 'working directory'):
                    state.publish_revision('480', source)
                self.assertFalse(any(state.revisions_dir.rglob('*')))
            finally:
                state.close()

    def test_manifest_applies_current_launch_policy_without_republishing(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            initial_state = self.make_state(root)
            try:
                source = root / 'download'
                source.mkdir()
                (source / 'game.exe').write_bytes(b'game-payload')
                published = initial_state.publish_revision('480', source)
                self.assertFalse(published['launchReady'])
            finally:
                initial_state.close()

            reviewed_state = SteamHostState(HostConfiguration(
                origin='https://steam-host.test:8443', host_id='test-host',
                data_dir=root / 'state',
                apps={'480': AppPolicy('480', 'Test game', 'game.exe', '.', True)},
                steamcmd=None, steam_user=None,
            ))
            try:
                self.assertTrue(reviewed_state.manifest('480')['launchReady'])

                revision_root = reviewed_state.revisions_dir / '480' / published['revision']
                with reviewed_state._manifest_path(revision_root).open('r', encoding='utf-8') as source_manifest:
                    self.assertFalse(json.load(source_manifest)['launchReady'])
            finally:
                reviewed_state.close()

    def test_content_file_named_like_the_legacy_manifest_is_preserved(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            state = self.make_state(root)
            try:
                source = root / 'download'
                source.mkdir()
                (source / 'game.exe').write_bytes(b'game-payload')
                (source / 'winehua-manifest.json').write_bytes(b'game-owned-data')
                manifest = state.publish_revision('480', source)
                artifacts = {item['relativePath']: item for item in manifest['artifacts']}
                self.assertIn('winehua-manifest.json', artifacts)
                self.assertEqual(
                    state.artifact_path(artifacts['winehua-manifest.json']['sha256']).read_bytes(),
                    b'game-owned-data',
                )
                self.assertEqual(state.manifest('480'), manifest)
            finally:
                state.close()

    def test_failed_verification_does_not_publish_an_incomplete_revision(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            state = self.make_state(root)
            try:
                source = root / 'download'
                source.mkdir()
                (source / 'game.exe').write_bytes(b'game-payload')
                with patch.object(state, '_enumerate_artifacts', side_effect=HostError('hash failed')):
                    with self.assertRaises(HostError):
                        state.publish_revision('480', source)
                self.assertIsNone(state._latest_revision('480'))
                self.assertTrue(source.is_dir())
            finally:
                state.close()

    def test_http_routes_require_a_pairing_token_for_content(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            state = self.make_state(root)
            server = None
            thread = None
            try:
                source = root / 'download'
                source.mkdir()
                (source / 'game.exe').write_bytes(b'game-payload')
                manifest = state.publish_revision('480', source)
                pairing_challenge, pairing_code = state.begin_pairing('b' * 24, 'WineHua')
                pairing = state.confirm_pairing(pairing_challenge['challengeId'], pairing_code, 'b' * 24)

                handler = type('TestHandler', (SteamHostRequestHandler,), {'state': state})
                server = ThreadingHTTPServer(('127.0.0.1', 0), handler)
                thread = threading.Thread(target=server.serve_forever, daemon=True)
                thread.start()
                base = f'http://127.0.0.1:{server.server_port}'

                challenge_request = Request(
                    base + '/v1/pair/begin',
                    data=b'{"protocolVersion":1,"clientNonce":"cccccccccccccccccccccccc","clientName":"WineHua"}',
                    headers={'Content-Type': 'application/json'}, method='POST')
                with urlopen(challenge_request) as response:
                    challenge = response.read().decode('utf-8')
                self.assertNotIn('verificationCode', challenge)

                headers = {'Authorization': 'Bearer ' + pairing['pairingToken']}
                with urlopen(Request(base + '/v1/library', headers=headers)) as response:
                    self.assertIn(b'480', response.read())
                with urlopen(Request(base + '/v1/games/480/manifest', headers=headers)) as response:
                    self.assertIn(b'games/480', response.read())
                artifact = manifest['artifacts'][0]
                with urlopen(Request(base + '/v1/artifacts/' + artifact['sha256'], headers=headers)) as response:
                    self.assertEqual(response.read(), b'game-payload')
            finally:
                if server is not None:
                    server.shutdown()
                    server.server_close()
                if thread is not None:
                    thread.join(timeout=2)
                state.close()

    def test_device_pair_revoke_invalidates_the_host_token(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            state = self.make_state(Path(temp))
            server = None
            thread = None
            try:
                challenge, code = state.begin_pairing('d' * 24, 'WineHua')
                pairing = state.confirm_pairing(challenge['challengeId'], code, 'd' * 24)
                handler = type('TestHandler', (SteamHostRequestHandler,), {'state': state})
                server = ThreadingHTTPServer(('127.0.0.1', 0), handler)
                thread = threading.Thread(target=server.serve_forever, daemon=True)
                thread.start()
                base = f'http://127.0.0.1:{server.server_port}'
                headers = {
                    'Authorization': 'Bearer ' + pairing['pairingToken'],
                    'Content-Type': 'application/json',
                }
                revoke = Request(base + '/v1/pair/revoke', data=b'{"protocolVersion":1}',
                                 headers=headers, method='POST')
                with urlopen(revoke) as response:
                    self.assertEqual(response.read(), b'{"revoked":true}')
                with self.assertRaises(HTTPError) as rejected:
                    urlopen(Request(base + '/v1/library', headers=headers))
                self.assertEqual(rejected.exception.code, 401)
                rejected.exception.close()
            finally:
                if server is not None:
                    server.shutdown()
                    server.server_close()
                if thread is not None:
                    thread.join(timeout=2)
                state.close()

    def test_reuses_active_download_job_for_the_same_app(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            state = self.make_state(Path(temp))
            try:
                with patch.object(state, 'steamcmd_download_ready', return_value=True), \
                     patch('steam_host.threading.Thread') as worker:
                    first = state.request_download('480')
                    duplicate = state.request_download('480')
                self.assertEqual(first['jobId'], duplicate['jobId'])
                self.assertEqual(first['state'], 'queued')
                worker.assert_called_once()
            finally:
                state.close()

    def test_marks_incomplete_jobs_failed_after_host_restart(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            first = self.make_state(root)
            try:
                with first.lock, first.db:
                    first.db.execute(
                        "INSERT INTO jobs(job_id, app_id, state) VALUES(?, ?, ?)",
                        ('restart-job-123', '480', 'running'),
                    )
            finally:
                first.close()
            second = self.make_state(root)
            try:
                job = second.job('restart-job-123', '480')
                self.assertEqual(job['state'], 'failed')
                self.assertEqual(job['errorCode'], 'host_restarted')
            finally:
                second.close()

    def test_interactive_steamcmd_login_does_not_accept_a_password_argument(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            steamcmd = Path(temp) / 'steamcmd'
            steamcmd.write_text('#!/bin/sh\n', encoding='utf-8')
            with patch('steam_host.subprocess.run', return_value=subprocess.CompletedProcess([], 0)) as run:
                result = main([
                    '--steamcmd-login',
                    '--steamcmd', str(steamcmd),
                    '--steam-user', 'host-account',
                ])
            self.assertEqual(result, 0)
            run.assert_called_once_with(
                [str(steamcmd.resolve()), '+login', 'host-account', '+quit'],
                cwd=str(steamcmd.resolve().parent), check=False,
            )

    def test_steamcmd_status_checks_the_same_session_without_interaction(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            steamcmd = root / 'steamcmd'
            steamcmd.write_text('#!/bin/sh\n', encoding='utf-8')
            with patch('steam_host.subprocess.run', return_value=subprocess.CompletedProcess([], 0)) as run:
                result = main([
                    '--steamcmd-status', '--data-dir', str(root / 'state'),
                    '--steamcmd', str(steamcmd), '--steam-user', 'host-account',
                ])
            self.assertEqual(result, 0)
            self.assertEqual(
                run.call_args.args[0],
                [str(steamcmd.resolve()), '+login', 'host-account', '+quit'],
            )
            self.assertEqual(run.call_args.kwargs['stdin'], subprocess.DEVNULL)
            self.assertEqual(run.call_args.kwargs['cwd'], str(steamcmd.resolve().parent))
            self.assertEqual(run.call_args.kwargs['timeout'], 120)

    def test_steamcmd_status_returns_nonzero_for_an_expired_session(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            steamcmd = root / 'steamcmd'
            steamcmd.write_text('#!/bin/sh\n', encoding='utf-8')
            with patch('steam_host.subprocess.run', return_value=subprocess.CompletedProcess([], 1)):
                result = main([
                    '--steamcmd-status', '--data-dir', str(root / 'state'),
                    '--steamcmd', str(steamcmd), '--steam-user', 'host-account',
                ])
            self.assertEqual(result, 1)

    def test_preflight_reports_a_ready_download_host_without_account_details(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            steamcmd = root / 'steamcmd'
            steamcmd.write_text('#!/bin/sh\n', encoding='utf-8')
            config = root / 'apps.json'
            config.write_text(json.dumps({
                'hostId': 'test-host',
                'apps': {
                    '480': {
                        'name': 'Test game',
                        'launchExeRelativePath': 'game.exe',
                        'workingDirectoryRelativePath': '.',
                        'launchReady': False,
                    },
                },
            }), encoding='utf-8')
            certificate = root / 'cert.pem'
            key = root / 'key.pem'
            certificate.write_text('certificate', encoding='ascii')
            key.write_text('key', encoding='ascii')
            stdout = io.StringIO()
            with patch('steam_host.ssl.SSLContext.load_cert_chain'), \
                 patch('steam_host.certificate_spki_pin', return_value='test-spki-pin'), \
                 patch('steam_host.validate_tls_origin') as validate_origin, \
                 patch('steam_host.subprocess.run', return_value=subprocess.CompletedProcess([], 0)):
                with redirect_stdout(stdout):
                    result = main([
                        '--preflight', '--data-dir', str(root / 'state'),
                        '--config', str(config), '--origin', 'https://steam-host.test:8443',
                        '--cert', str(certificate), '--key', str(key),
                        '--steamcmd', str(steamcmd), '--steam-user', 'host-account',
                    ])
            self.assertEqual(result, 0)
            report = json.loads(stdout.getvalue())
            self.assertTrue(report['downloadReady'])
            self.assertEqual(report['steamcmd']['sessionState'], 'ready')
            self.assertEqual(report['steamcmd']['targetPlatform'], 'windows')
            self.assertEqual(report['tls']['publicKeyPin'], 'test-spki-pin')
            self.assertTrue(report['tls']['originHostnameChecked'])
            validate_origin.assert_called_once()
            self.assertNotIn('host-account', stdout.getvalue())

    def test_preflight_rejects_an_expired_steamcmd_session(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            steamcmd = root / 'steamcmd'
            steamcmd.write_text('#!/bin/sh\n', encoding='utf-8')
            config = root / 'apps.json'
            config.write_text(json.dumps({
                'hostId': 'test-host',
                'apps': {
                    '480': {
                        'name': 'Test game',
                        'launchExeRelativePath': 'game.exe',
                        'workingDirectoryRelativePath': '.',
                        'launchReady': False,
                    },
                },
            }), encoding='utf-8')
            certificate = root / 'cert.pem'
            key = root / 'key.pem'
            certificate.write_text('certificate', encoding='ascii')
            key.write_text('key', encoding='ascii')
            stdout = io.StringIO()
            with patch('steam_host.ssl.SSLContext.load_cert_chain'), \
                 patch('steam_host.certificate_spki_pin', return_value='test-spki-pin'), \
                 patch('steam_host.validate_tls_origin') as validate_origin, \
                 patch('steam_host.subprocess.run', return_value=subprocess.CompletedProcess([], 1)):
                with redirect_stdout(stdout):
                    result = main([
                        '--preflight', '--data-dir', str(root / 'state'),
                        '--config', str(config), '--origin', 'https://steam-host.test:8443',
                        '--cert', str(certificate), '--key', str(key),
                        '--steamcmd', str(steamcmd), '--steam-user', 'host-account',
                    ])
            self.assertEqual(result, 1)
            report = json.loads(stdout.getvalue())
            self.assertFalse(report['downloadReady'])
            self.assertEqual(report['steamcmd']['sessionState'], 'login_required')
            validate_origin.assert_called_once()

    def test_probe_app_lists_windows_executables_without_preserving_depot_content(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            steamcmd = root / 'steamcmd'
            steamcmd.write_text('#!/bin/sh\n', encoding='utf-8')
            config = root / 'apps.json'
            config.write_text(json.dumps({
                'hostId': 'test-host',
                'apps': {},
            }), encoding='utf-8')
            stdout = io.StringIO()

            def fake_run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
                if '+app_update' in command:
                    probe = Path(command[command.index('+force_install_dir') + 1])
                    (probe / 'bin').mkdir()
                    (probe / 'bin' / 'Launcher.EXE').write_bytes(b'launcher')
                    (probe / 'README.txt').write_text('not executable', encoding='utf-8')
                return subprocess.CompletedProcess(command, 0)

            with patch('steam_host.subprocess.run', side_effect=fake_run) as run:
                with redirect_stdout(stdout):
                    result = main([
                        '--probe-app', '480', '--data-dir', str(root / 'state'),
                        '--config', str(config), '--origin', 'https://steam-host.test:8443',
                        '--steamcmd', str(steamcmd), '--steam-user', 'host-account',
                    ])
            self.assertEqual(result, 0)
            report = json.loads(stdout.getvalue())
            self.assertTrue(report['probeReady'])
            self.assertEqual(report['platform'], 'windows')
            self.assertEqual(report['executables'], [
                {'relativePath': 'bin/Launcher.EXE', 'sizeBytes': len(b'launcher')},
            ])
            probe_command = next(call.args[0] for call in run.call_args_list if '+app_update' in call.args[0])
            probe_path = Path(probe_command[probe_command.index('+force_install_dir') + 1])
            self.assertFalse(probe_path.exists())
            self.assertNotIn('host-account', stdout.getvalue())


if __name__ == '__main__':
    unittest.main()
