import { window } from '@kit.ArkUI';
import testNapi from 'libentry.so';
import { HgsService, HgsServiceEvent, HgsServiceWindow, HgsServiceWindowHost, HgsSession } from 'hgs';

export interface X11ImeTarget {
  session: HgsSession;
  windowId: string;
}

// WineHua observes desktop and IME state while the HGS host owns the windows.
export class X11ProbeWindowHost {
  private stage: window.WindowStage | null = null;
  private session: HgsSession | null = null;
  private subscription: number = 0;
  private host: HgsServiceWindowHost | null = null;
  private imeTarget: X11ImeTarget | null = null;
  private imeListener: ((target: X11ImeTarget | null) => void) | null = null;
  private desktopPid: number = 0;
  private desktopReady: boolean = false;
  private desktopListener: ((ready: boolean) => void) | null = null;

  getSession(): HgsSession | null {
    return this.session;
  }

  findSmokeWindow(title?: string): HgsServiceWindow | undefined {
    const windows = this.session?.windows().filter((snapshot: HgsServiceWindow): boolean =>
      snapshot.visible && snapshot.protocol === 'x11' && snapshot.role === 'toplevel') ?? [];
    if (title) return windows.find((snapshot: HgsServiceWindow): boolean => snapshot.title.includes(title));
    if (this.desktopPid > 0) {
      return windows.find((snapshot: HgsServiceWindow): boolean =>
        snapshot.title.startsWith('shell - ') && testNapi.matchX11DesktopProcess(this.desktopPid, snapshot.clientPid));
    }
    return windows.length === 1 ? windows[0] : undefined;
  }

  setDesktopPid(pid: number): void {
    this.desktopPid = pid;
    this.updateDesktopReady();
  }

  private updateDesktopReady(): void {
    let ready = false;
    if (this.desktopPid > 0 && this.session && this.host) {
      ready = this.session.windows().some((snapshot: HgsServiceWindow): boolean =>
        snapshot.protocol === 'x11' && snapshot.framePresented && snapshot.title.startsWith('shell - ') &&
        (this.host?.isWindowShown(snapshot.id) ?? false) &&
        testNapi.matchX11DesktopProcess(this.desktopPid, snapshot.clientPid));
    }
    if (ready !== this.desktopReady) {
      this.desktopReady = ready;
      this.desktopListener?.(ready);
    }
  }

  setImeListener(listener: ((target: X11ImeTarget | null) => void) | null): void {
    this.imeListener = listener;
    listener?.(this.imeTarget);
  }

  private setImeTarget(target: X11ImeTarget | null): void {
    if (this.imeTarget?.windowId === target?.windowId) return;
    this.imeTarget = target;
    this.imeListener?.(target);
  }

  setStage(stage: window.WindowStage | null): void {
    if (this.stage === stage) return;
    this.setImeTarget(null);
    this.stage = stage;
    this.startHost();
  }

  private startHost(): void {
    this.host?.stop();
    this.host = null;
    const session = this.session;
    const stage = this.stage;
    if (!session || !stage) return;
    const host = new HgsServiceWindowHost(session, stage, 'pages/X11ProbeWindow', 1);
    this.host = host;
    host.setWindowShownListener((_snapshot: HgsServiceWindow): void => this.updateDesktopReady());
    host.start();
  }

  start(session: HgsSession, desktopListener: ((ready: boolean) => void) | null = null): void {
    this.stop();
    this.session = session;
    this.desktopListener = desktopListener;
    const service = HgsService.create();
    this.subscription = service.onEvent((event: HgsServiceEvent): void => {
      if (event.epoch !== session.epoch) return;
      if ((event.kind === 'backend-disconnected' && event.origin === 'xwayland') ||
        event.state === 'stopping' || event.state === 'stopped') {
        this.stop();
      } else if (event.window.protocol === 'x11') {
        const snapshot = event.window;
        if (event.kind === 'destroyed' && this.imeTarget?.windowId === snapshot.id) {
          this.setImeTarget(null);
        } else if (event.kind === 'created' || event.kind === 'changed') {
          if (snapshot.visible && snapshot.textInputActive) {
            this.setImeTarget({ session: session, windowId: snapshot.id });
          } else if (this.imeTarget?.windowId === snapshot.id) {
            this.setImeTarget(null);
          }
        }
        this.updateDesktopReady();
      }
    });
    this.startHost();
  }

  stop(): void {
    if (this.subscription !== 0) HgsService.create().offEvent(this.subscription);
    this.subscription = 0;
    this.session = null;
    this.setDesktopPid(0);
    this.desktopListener = null;
    this.setImeTarget(null);
    this.host?.stop();
    this.host = null;
  }
}
