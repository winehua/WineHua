import { SteamHostDownloadJob, SteamHostLibraryEntry } from '../service/SteamHostProtocol';
import { SteamHostService } from '../service/SteamHostService';

@Component
export struct SteamHostPanel {
  @Link visible: boolean;

  private readonly service: SteamHostService = SteamHostService.getInstance();
  @State private origin: string = '';
  @State private certificatePin: string = '';
  @State private challengeId: string = '';
  @State private verificationCode: string = '';
  @State private paired: boolean = false;
  @State private hostId: string = '';
  @State private capabilitiesReady: boolean = false;
  @State private busy: boolean = false;
  @State private status: string = '';
  @State private library: SteamHostLibraryEntry[] = [];
  @State private activeJob: SteamHostDownloadJob | null = null;
  @State private localReadyApps: string[] = [];

  aboutToAppear(): void {
    this.refreshHost();
  }

  private refreshHost = async (): Promise<void> => {
    if (this.busy) return;
    this.busy = true;
    try {
      const summary = this.service.pairingSummary();
      this.paired = summary.paired;
      this.hostId = summary.hostId;
      if (summary.origin.length > 0) this.origin = summary.origin;
      if (!summary.paired) {
        this.capabilitiesReady = false;
        this.library = [];
        this.status = '请输入宿主地址和证书指纹以开始配对';
        return;
      }
      this.capabilitiesReady = await this.service.getCapabilities();
      const library = await this.service.getLibrary();
      this.library = library ?? [];
      this.status = this.capabilitiesReady ? `已连接 ${summary.hostId}` : '宿主不可用或能力不完整';
    } catch (_) {
      this.capabilitiesReady = false;
      this.status = '读取宿主状态失败';
    } finally {
      this.busy = false;
    }
  }

  private beginPairing = async (): Promise<void> => {
    if (this.busy) return;
    this.busy = true;
    this.status = '正在创建配对请求...';
    try {
      const challenge = await this.service.beginPairing(this.origin, this.certificatePin);
      if (challenge === null) {
        this.status = '配对请求被拒绝，请检查 HTTPS 地址和证书指纹';
        return;
      }
      this.challengeId = challenge.challengeId;
      this.hostId = challenge.hostId;
      this.status = '请在宿主终端确认代码后输入';
    } catch (_) {
      this.status = '无法连接到宿主';
    } finally {
      this.busy = false;
    }
  }

  private confirmPairing = async (): Promise<void> => {
    if (this.busy || this.challengeId.length === 0) return;
    this.busy = true;
    this.status = '正在确认配对...';
    try {
      if (!await this.service.confirmPairing(this.origin, this.certificatePin,
        this.challengeId, this.verificationCode)) {
        this.status = '配对确认失败或挑战已过期';
        return;
      }
      this.challengeId = '';
      this.verificationCode = '';
      this.status = '配对成功';
      this.busy = false;
      await this.refreshHost();
    } catch (_) {
      this.status = '配对确认失败';
    } finally {
      this.busy = false;
    }
  }

  private revokePairing = async (): Promise<void> => {
    if (this.busy) return;
    this.busy = true;
    const hostRevoked = await this.service.revokePairing();
    this.paired = false;
    this.hostId = '';
    this.capabilitiesReady = false;
    this.library = [];
    this.challengeId = '';
    this.verificationCode = '';
    this.status = hostRevoked ? '已撤销宿主配对' : '已移除本机配对，宿主未确认撤销';
    this.busy = false;
  }

  private requestDownload = async (entry: SteamHostLibraryEntry): Promise<void> => {
    if (this.busy || !entry.downloadable) return;
    this.busy = true;
    this.status = `正在请求宿主下载 ${entry.name}...`;
    try {
      const job = await this.service.requestDownload(entry.appId);
      if (job === null) {
        this.status = '宿主拒绝下载请求';
        return;
      }
      this.activeJob = job;
      this.status = this.formatJob(job);
    } catch (_) {
      this.status = '下载请求失败';
    } finally {
      this.busy = false;
    }
  }

  private refreshDownload = async (): Promise<void> => {
    const active = this.activeJob;
    if (this.busy || active === null) return;
    let refreshLibrary = false;
    this.busy = true;
    try {
      const job = await this.service.getDownloadJob(active.appId, active.jobId);
      if (job === null) {
        this.status = '宿主下载任务不可用';
        return;
      }
      this.activeJob = job;
      this.status = this.formatJob(job);
      refreshLibrary = job.state === 'complete';
    } catch (_) {
      this.status = '无法读取下载进度';
    } finally {
      this.busy = false;
    }
    if (refreshLibrary) await this.refreshHost();
  }

  private install = async (entry: SteamHostLibraryEntry): Promise<void> => {
    if (this.busy || !entry.launchReady) return;
    this.busy = true;
    this.status = `正在验证并安装 ${entry.name}...`;
    try {
      const installed = await this.service.installVerifiedManifest(entry.appId);
      if (!installed.installed) {
        this.status = `安装失败: ${installed.error}`;
        return;
      }
      this.addLocalReady(entry.appId);
      this.status = `${entry.name} 已安装到独立 Wine 容器`;
    } catch (_) {
      this.status = '设备安装失败';
    } finally {
      this.busy = false;
    }
  }

  private launch = async (entry: SteamHostLibraryEntry): Promise<void> => {
    if (this.busy || !this.isLocalReady(entry.appId)) return;
    this.busy = true;
    this.status = `正在启动 ${entry.name}...`;
    try {
      const launched = await this.service.launchInstalledApp(entry.appId);
      this.status = launched.launched ? `${entry.name} 已启动` : `启动失败: ${launched.error}`;
    } catch (_) {
      this.status = '启动失败';
    } finally {
      this.busy = false;
    }
  }

  private addLocalReady(appId: string): void {
    if (this.localReadyApps.indexOf(appId) >= 0) return;
    this.localReadyApps = this.localReadyApps.concat([appId]);
  }

  private isLocalReady(appId: string): boolean {
    return this.localReadyApps.indexOf(appId) >= 0 || this.service.hasInstalledApp(appId);
  }

  private formatJob(job: SteamHostDownloadJob): string {
    const total = job.totalBytes > 0 ? `${job.transferredBytes}/${job.totalBytes}` : `${job.transferredBytes}`;
    return `宿主下载 ${job.state}: ${total}`;
  }

  build() {
    Stack() {
      Column() {
        Row() {
          Column() {
            Text('Steam 宿主')
              .fontSize(22).fontWeight(FontWeight.Medium).fontColor('#f4f7fb')
            Text(this.paired ? `已配对: ${this.hostId}` : '未配对')
              .fontSize(13).fontColor(this.capabilitiesReady ? '#8f8' : '#9aa5b2')
          }
          .layoutWeight(1).alignItems(HorizontalAlign.Start)
          Button('关闭')
            .height(36).backgroundColor('#333')
            .onClick(() => { this.visible = false; })
        }
        .width('100%').padding({ left: 20, right: 20, top: 16, bottom: 12 })

        Divider().color('#2a2a2a').width('100%')

        Scroll() {
          Column({ space: 10 }) {
            if (!this.paired) {
              Text('宿主地址')
                .fontSize(13).fontColor('#aeb8c6').width('100%')
              TextInput({ text: this.origin, placeholder: 'https://steam-host.local:8443' })
                .width('100%').height(42)
                .backgroundColor('#1d2229').fontColor('#f4f7fb')
                .onChange((value: string) => { this.origin = value; })
              Text('SPKI SHA-256 指纹')
                .fontSize(13).fontColor('#aeb8c6').width('100%')
              TextInput({ text: this.certificatePin, placeholder: 'Base64 SHA-256' })
                .width('100%').height(42)
                .backgroundColor('#1d2229').fontColor('#f4f7fb')
                .onChange((value: string) => { this.certificatePin = value; })
              Button('开始配对')
                .width('100%').height(40).backgroundColor('#285')
                .enabled(!this.busy && this.origin.length > 0 && this.certificatePin.length > 0)
                .onClick(this.beginPairing)
              if (this.challengeId.length > 0) {
                Text('宿主确认代码')
                  .fontSize(13).fontColor('#aeb8c6').width('100%')
                TextInput({ text: this.verificationCode, placeholder: '输入宿主终端显示的代码' })
                  .width('100%').height(42)
                  .backgroundColor('#1d2229').fontColor('#f4f7fb')
                  .onChange((value: string) => { this.verificationCode = value; })
                Button('确认配对')
                  .width('100%').height(40).backgroundColor('#285')
                  .enabled(!this.busy && this.verificationCode.length > 0)
                  .onClick(this.confirmPairing)
              }
            } else {
              Row({ space: 8 }) {
                Button('刷新宿主')
                  .layoutWeight(1).height(38).backgroundColor('#246')
                  .enabled(!this.busy).onClick(this.refreshHost)
                Button('移除配对')
                  .layoutWeight(1).height(38).backgroundColor('#633')
                  .enabled(!this.busy).onClick(this.revokePairing)
              }
              .width('100%')

              if (this.activeJob !== null) {
                Row({ space: 8 }) {
                  Text(this.formatJob(this.activeJob))
                    .layoutWeight(1).fontSize(12).fontColor('#8cf').maxLines(1)
                    .textOverflow({ overflow: TextOverflow.Ellipsis })
                  Button('刷新进度')
                    .height(34).backgroundColor('#333').enabled(!this.busy)
                    .onClick(this.refreshDownload)
                }
                .width('100%').padding({ top: 4, bottom: 4 })
              }

              Text('库')
                .fontSize(15).fontWeight(FontWeight.Medium).fontColor('#dbe4ef')
                .width('100%').padding({ top: 8 })
              if (this.library.length === 0) {
                Text(this.capabilitiesReady ? '宿主库中没有可用条目' : '正在等待宿主响应')
                  .fontSize(13).fontColor('#778390').width('100%').padding({ top: 8 })
              }
              ForEach(this.library, (entry: SteamHostLibraryEntry) => {
                Column({ space: 7 }) {
                  Row() {
                    Column() {
                      Text(entry.name)
                        .fontSize(15).fontColor('#f4f7fb').maxLines(1)
                        .textOverflow({ overflow: TextOverflow.Ellipsis })
                      Text(`AppID ${entry.appId}`)
                        .fontSize(12).fontColor('#8b96a3')
                    }
                    .layoutWeight(1).alignItems(HorizontalAlign.Start)
                    Text(entry.launchReady ? '可启动' : '需宿主审核')
                      .fontSize(12).fontColor(entry.launchReady ? '#8f8' : '#e2b26d')
                  }
                  .width('100%')
                  Row({ space: 7 }) {
                    Button('宿主下载')
                      .layoutWeight(1).height(34).backgroundColor('#246')
                      .enabled(!this.busy && entry.downloadable)
                      .onClick(() => this.requestDownload(entry))
                    Button(this.isLocalReady(entry.appId) ? '重新安装' : '安装到设备')
                      .layoutWeight(1).height(34).backgroundColor('#285')
                      .enabled(!this.busy && entry.launchReady && entry.installed)
                      .onClick(() => this.install(entry))
                    Button('启动')
                      .layoutWeight(1).height(34).backgroundColor('#394')
                      .enabled(!this.busy && this.isLocalReady(entry.appId))
                      .onClick(() => this.launch(entry))
                  }
                  .width('100%')
                }
                .width('100%').padding({ top: 10, bottom: 10 })
                Divider().color('#2a2a2a').width('100%')
              }, (entry: SteamHostLibraryEntry) => entry.appId)
            }
          }
          .width('100%').padding({ left: 20, right: 20, top: 16, bottom: 20 })
        }
        .width('100%').layoutWeight(1).scrollBar(BarState.Auto)

        if (this.busy) {
          Row({ space: 8 }) {
            LoadingProgress().width(16).height(16).color('#7db8ff')
            Text(this.status || '处理中...').fontSize(13).fontColor('#aeb8c6')
          }
          .width('100%').padding({ left: 20, right: 20, top: 10, bottom: 12 })
        } else if (this.status.length > 0) {
          Text(this.status)
            .fontSize(13).fontColor('#aeb8c6').width('100%')
            .padding({ left: 20, right: 20, top: 10, bottom: 12 })
            .maxLines(2).textOverflow({ overflow: TextOverflow.Ellipsis })
        }
      }
      .width('92%').height('90%').backgroundColor('#14181e')
    }
    .width('100%').height('100%').backgroundColor('#000000aa')
  }
}
