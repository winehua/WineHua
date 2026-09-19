import { hilog } from '@kit.PerformanceAnalysisKit';
import { SteamHostService } from './SteamHostService';

const DOMAIN = 0x0000;
const TAG = 'SteamHost';

interface SteamHostActionRequest {
  action: string;
  values: Record<string, string>;
}

// Controlled Want entrypoint for the Host content workflow. This follows the
// existing smoke/game hooks so first-device acceptance can exercise pairing,
// download, install and launch without exposing Steam credentials to OHOS.
export class SteamHostHook {
  private static instance: SteamHostHook | null = null;

  static getInstance(): SteamHostHook {
    if (SteamHostHook.instance === null) SteamHostHook.instance = new SteamHostHook();
    return SteamHostHook.instance;
  }

  private actionBusy: boolean = false;
  private wineEnvironmentReady: boolean = false;
  private pendingWineAction: SteamHostActionRequest | null = null;

  applyWant(parameters: Record<string, Object> | undefined): void {
    const value = (key: string): string => {
      if (!parameters || parameters[key] === undefined || parameters[key] === null) return '';
      return `${parameters[key]}`;
    };
    if (value('winehua.mode') !== 'steam-host') return;
    const request: SteamHostActionRequest = {
      action: value('winehua.steam_action'),
      values: {
        'winehua.host_origin': value('winehua.host_origin'),
        'winehua.certificate_pin': value('winehua.certificate_pin'),
        'winehua.challenge_id': value('winehua.challenge_id'),
        'winehua.verification_code': value('winehua.verification_code'),
        'winehua.app_id': value('winehua.app_id'),
        'winehua.job_id': value('winehua.job_id')
      }
    };
    if (this.actionBusy) {
      hilog.warn(DOMAIN, TAG, 'ignoring action while another Host action is active: %{public}s', request.action);
      return;
    }
    if (this.requiresWineEnvironment(request.action) && !this.wineEnvironmentReady) {
      this.pendingWineAction = request;
      hilog.info(DOMAIN, TAG, 'deferred Wine action until environment initialization: %{public}s', request.action);
      return;
    }
    this.startAction(request);
  }

  // Index calls this after WineEnvService has registered all native callbacks.
  // A cold-start install/launch Want must not race that registration.
  attachWineEnvironment(): void {
    this.wineEnvironmentReady = true;
    const request = this.pendingWineAction;
    if (request === null || this.actionBusy) return;
    this.pendingWineAction = null;
    this.startAction(request);
  }

  private requiresWineEnvironment(action: string): boolean {
    return action === 'install' || action === 'launch' || action === 'install-launch';
  }

  private startAction(request: SteamHostActionRequest): void {
    this.actionBusy = true;
    void this.runAction(request);
  }

  private async runAction(request: SteamHostActionRequest): Promise<void> {
    const action = request.action;
    const value = (key: string): string => request.values[key] ?? '';
    const service = SteamHostService.getInstance();
    const appId = value('winehua.app_id');
    try {
      switch (action) {
        case 'pair-begin': {
          const challenge = await service.beginPairing(
            value('winehua.host_origin'), value('winehua.certificate_pin'));
          if (challenge === null) {
            this.failure(action, 'pairing challenge rejected');
          } else {
            hilog.info(DOMAIN, TAG,
              'action=%{public}s status=challenge host=%{public}s challenge=%{public}s expires=%{public}d',
              action, challenge.hostId, challenge.challengeId, challenge.expiresAtMs);
          }
          return;
        }
        case 'pair-confirm': {
          const paired = await service.confirmPairing(value('winehua.host_origin'),
            value('winehua.certificate_pin'), value('winehua.challenge_id'),
            value('winehua.verification_code'));
          if (!paired) {
            this.failure(action, 'pairing confirmation rejected');
          } else {
            const summary = service.pairingSummary();
            hilog.info(DOMAIN, TAG, 'action=%{public}s status=paired host=%{public}s',
              action, summary.hostId);
          }
          return;
        }
        case 'revoke': {
          const hostRevoked = await service.revokePairing();
          hilog.info(DOMAIN, TAG, 'action=%{public}s status=revoked host=%{public}s', action, hostRevoked);
          return;
        }
        case 'capabilities': {
          const available = await service.getCapabilities();
          if (!available) this.failure(action, 'paired Host capabilities unavailable');
          else hilog.info(DOMAIN, TAG, 'action=%{public}s status=ready', action);
          return;
        }
        case 'library': {
          const library = await service.getLibrary();
          if (library === null) {
            this.failure(action, 'library unavailable');
          } else {
            let ready = 0;
            for (const item of library) if (item.launchReady) ready += 1;
            hilog.info(DOMAIN, TAG, 'action=%{public}s status=ok entries=%{public}d launchReady=%{public}d',
              action, library.length, ready);
          }
          return;
        }
        case 'download': {
          const job = await service.requestDownload(appId);
          if (job === null) this.failure(action, 'download request rejected');
          else hilog.info(DOMAIN, TAG, 'action=%{public}s status=%{public}s app=%{public}s job=%{public}s',
            action, job.state, job.appId, job.jobId);
          return;
        }
        case 'download-status': {
          const job = await service.getDownloadJob(appId, value('winehua.job_id'));
          if (job === null) this.failure(action, 'download job unavailable');
          else hilog.info(DOMAIN, TAG,
            'action=%{public}s status=%{public}s app=%{public}s job=%{public}s bytes=%{public}d/%{public}d',
            action, job.state, job.appId, job.jobId, job.transferredBytes, job.totalBytes);
          return;
        }
        case 'install': {
          const installed = await service.installVerifiedManifest(appId);
          if (!installed.installed) this.failure(action, installed.error);
          else hilog.info(DOMAIN, TAG, 'action=%{public}s status=installed app=%{public}s revision=%{public}s',
            action, installed.appId, installed.revision);
          return;
        }
        case 'launch': {
          const launched = await service.launchInstalledApp(appId);
          if (!launched.launched) this.failure(action, launched.error);
          else hilog.info(DOMAIN, TAG, 'action=%{public}s status=launched app=%{public}s pid=%{public}d',
            action, launched.appId, launched.pid);
          return;
        }
        case 'install-launch': {
          const launched = await service.installAndLaunch(appId);
          if (!launched.launched) this.failure(action, launched.error);
          else hilog.info(DOMAIN, TAG, 'action=%{public}s status=launched app=%{public}s pid=%{public}d',
            action, launched.appId, launched.pid);
          return;
        }
        default:
          this.failure(action, 'unsupported steam-host action');
      }
    } catch (_) {
      this.failure(action, 'unexpected Host action error');
    } finally {
      this.actionBusy = false;
    }
  }

  private failure(action: string, reason: string): void {
    // Do not print origins, pins, pairing tokens, passwords, or Steam Guard codes.
    hilog.error(DOMAIN, TAG, 'action=%{public}s status=failed reason=%{public}s', action, reason);
  }
}
