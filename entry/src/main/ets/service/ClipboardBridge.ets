import { abilityAccessCtrl, common, Permissions } from '@kit.AbilityKit';
import { pasteboard, BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';

const DOMAIN = 0x0000;
const TAG = 'ClipboardBridge';
const READ_PERMISSION: Permissions = 'ohos.permission.READ_PASTEBOARD';

/**
 * 系统剪贴板 <-> Wine (Xwayland CLIPBOARD) 双向文本桥。
 *
 * guest -> host: 原生侧读到 X11 剪贴板文本后回调 onGuestClipboard，
 * 这里写入系统 pasteboard。
 * host -> guest: 系统 pasteboard update 时读主文本，交给原生侧设
 * seat selection；xwm 会把它桥成 Xwayland CLIPBOARD。
 *
 * lastText 用于打断自身写入触发的 update 回声，避免双向来回推送。
 */
export class ClipboardBridge {
  private static instance: ClipboardBridge | null = null;

  static getInstance(): ClipboardBridge {
    if (!ClipboardBridge.instance) {
      ClipboardBridge.instance = new ClipboardBridge();
    }
    return ClipboardBridge.instance;
  }

  private initialized: boolean = false;
  private canRead: boolean = false;
  private lastText: string = '';
  private board: pasteboard.SystemPasteboard | null = null;

  init(context: common.UIAbilityContext): void {
    if (this.initialized) {
      return;
    }
    this.initialized = true;
    try {
      this.board = pasteboard.getSystemPasteboard();
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'getSystemPasteboard failed: %{public}s', JSON.stringify(e));
    }
    try {
      testNapi.setClipboardUpdateCallback((text: string) => this.onGuestClipboard(text));
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'setClipboardUpdateCallback failed: %{public}s', JSON.stringify(e));
    }
    if (this.board) {
      try {
        this.board.on('update', () => this.syncHostClipboard());
      } catch (e) {
        hilog.error(DOMAIN, TAG, 'pasteboard update listener failed: %{public}s', JSON.stringify(e));
      }
    }
    this.requestPermission(context).then(() => this.syncHostClipboard());
  }

  private async requestPermission(context: common.UIAbilityContext): Promise<void> {
    try {
      const atManager = abilityAccessCtrl.createAtManager();
      const result = await atManager.requestPermissionsFromUser(context, [READ_PERMISSION]);
      this.canRead = result.authResults.length > 0 && result.authResults[0] === 0;
      hilog.info(DOMAIN, TAG, 'READ_PASTEBOARD granted=%{public}d', this.canRead ? 1 : 0);
    } catch (e) {
      const err = e as BusinessError;
      hilog.error(DOMAIN, TAG, 'request READ_PASTEBOARD failed: %{public}d %{public}s',
        err.code, err.message);
    }
  }

  /** 读系统剪贴板并推给 guest。系统剪贴板在应用后台变化时，回前台重读。 */
  syncHostClipboard(): void {
    if (!this.canRead || !this.board) {
      return;
    }
    this.board.getData().then((data: pasteboard.PasteData) => {
      const text = data.getPrimaryText();
      if (!text || text === this.lastText) {
        return;
      }
      this.lastText = text;
      testNapi.setClipboardText(text);
      hilog.info(DOMAIN, TAG, 'host -> guest, %{public}d chars', text.length);
    }).catch((e: BusinessError) => {
      hilog.warn(DOMAIN, TAG, 'read pasteboard failed: %{public}d %{public}s', e.code, e.message);
    });
  }

  private onGuestClipboard(text: string): void {
    if (!text || text === this.lastText || !this.board) {
      return;
    }
    this.lastText = text;
    try {
      const data = pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, text);
      this.board.setData(data).then(() => {
        hilog.info(DOMAIN, TAG, 'guest -> host, %{public}d chars', text.length);
      }).catch((e: BusinessError) => {
        hilog.warn(DOMAIN, TAG, 'write pasteboard failed: %{public}d %{public}s', e.code, e.message);
      });
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'createData failed: %{public}s', JSON.stringify(e));
    }
  }
}
