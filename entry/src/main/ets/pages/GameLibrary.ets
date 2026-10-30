import { common } from '@kit.AbilityKit';
import { deviceInfo } from '@kit.BasicServicesKit';
import { GameEntry, GameProfile, GAME_BACKENDS, GAME_COMPAT_MODES, GAME_RESOLUTIONS, GAME_OPENGL_RENDERERS,
  defaultGameProfile, expandGameEntries } from '../common/GameLibraryModel';
import { GameLibraryService } from '../service/GameLibraryService';
import { WineEnvService } from '../service/WineEnvService';
import { WineWindowManager } from '../service/WineWindowManager';

const BACKENDS: SelectOption[] = [{ value: '跟随全局设置' }, { value: 'WineD3D / OpenGL' },
  { value: 'DXVK 1.10.3 · D3D10/11' }, { value: 'DXVK 1.10.3 · D3D9/10/11' },
  { value: 'DXVK 2.6.2' }, { value: 'VKD3D 2.6 · D3D12（实验）' }];
const COMPAT: SelectOption[] = [{ value: '默认（保留程序专用优化）' }, { value: '稳定优先' },
  { value: '保守兼容' }, { value: '平衡' }, { value: '性能优先' }];
const OPENGL: SelectOption[] = [{ value: '自动（程序专用优化）' }, { value: 'VirGL / OpenGL' },
  { value: 'Zink / Vulkan（已验证手机）' }];
const RESOLUTIONS: SelectOption[] = [{ value: '跟随全局设置' }, { value: '跟随屏幕' }, { value: '1024×768 · 4:3' }];

@Entry
@Component
struct GameLibrary {
  private library = GameLibraryService.getInstance();
  private env = WineEnvService.getInstance();
  @State games: GameEntry[] = [];
  @State programs: GameEntry[] = [];
  @State showPrograms: boolean = false;
  @State query: string = '';
  @State root: string = '';
  @State message: string = '';
  @State scanning: boolean = false;
  @State launchingId: string = '';
  @State selectedId: string = '';
  @State editName: string = '';
  @State executable: string = '';
  @State argumentsText: string = '';
  @State workingDirectory: string = '';
  @State backendIndex: number = 0;
  @State openglIndex: number = 0;
  @State compatIndex: number = 0;
  @State resolutionIndex: number = 0;
  @State environmentText: string = '';
  @State dirty: boolean = false;
  @State engineStatus: string = '';
  @State desktopAvailable: boolean = false;

  private mirror = (): void => {
    this.games = this.library.entries.slice();
    this.programs = expandGameEntries(this.games);
    this.root = this.library.root;
    this.scanning = this.library.scanning;
    this.launchingId = this.library.launchingId;
    this.message = this.library.message;
    if (!this.currentGame()) {
      const visible = this.showPrograms ? this.programs : this.games;
      if (visible.length) this.loadGame(visible[0]);
    }
  };
  private mirrorEnvironment = (): void => {
    this.engineStatus = this.env.statusText;
    this.desktopAvailable = this.env.desktopActive;
  };

  aboutToAppear(): void {
    this.library.init(getContext(this));
    this.library.subscribe(this.mirror);
    this.env.subscribe(this.mirrorEnvironment);
    this.mirror(); this.mirrorEnvironment();
    this.library.refresh();
  }
  aboutToDisappear(): void {
    this.library.unsubscribe(this.mirror);
    this.env.unsubscribe(this.mirrorEnvironment);
  }

  private currentGame(): GameEntry | null {
    return this.games.concat(this.programs).find((game: GameEntry) => game.id === this.selectedId) || null;
  }
  private visibleGames(): GameEntry[] {
    const query = this.query.trim().toLowerCase();
    const games = this.showPrograms ? this.programs : this.games;
    return games.filter((game: GameEntry) => !query ||
      this.library.getProfile(game).name.toLowerCase().indexOf(query) >= 0 ||
      game.directory.toLowerCase().indexOf(query) >= 0 ||
      game.executables.some((path: string) => path.toLowerCase().indexOf(query) >= 0));
  }
  private loadProfile(profile: GameProfile): void {
    this.editName = profile.name; this.executable = profile.executable;
    this.argumentsText = profile.argumentsText; this.workingDirectory = profile.workingDirectory;
    this.backendIndex = Math.max(0, GAME_BACKENDS.indexOf(profile.d3dBackend));
    this.openglIndex = Math.max(0, GAME_OPENGL_RENDERERS.indexOf(profile.openglRenderer));
    this.compatIndex = Math.max(0, GAME_COMPAT_MODES.indexOf(profile.compatMode));
    this.resolutionIndex = Math.max(0, GAME_RESOLUTIONS.indexOf(profile.resolution));
    this.environmentText = profile.environmentText; this.dirty = false;
  }
  private setListingMode(programs: boolean): void {
    if (programs === this.showPrograms) return;
    const change = (): void => {
      this.showPrograms = programs;
      const visible = this.visibleGames();
      if (visible.length) this.loadGame(visible[0]);
      else { this.selectedId = ''; this.dirty = false; }
    };
    if (!this.dirty) { change(); return; }
    AlertDialog.show({ title: '设置尚未保存', message: '切换视图会放弃未保存的修改。',
      primaryButton: { value: '继续编辑', action: () => {} },
      secondaryButton: { value: '放弃并切换', action: change } });
  }
  private loadGame(game: GameEntry): void {
    this.selectedId = game.id;
    this.loadProfile(this.library.getProfile(game));
  }
  private chooseGame(game: GameEntry): void {
    if (this.selectedId === game.id) return;
    if (!this.dirty) { this.loadGame(game); return; }
    AlertDialog.show({ title: '设置尚未保存', message: '切换游戏会放弃当前未保存的修改。',
      primaryButton: { value: '继续编辑', action: () => {} },
      secondaryButton: { value: '放弃并切换', action: () => { this.loadGame(game); } } });
  }
  private draft(game: GameEntry): GameProfile {
    return { id: game.id, name: this.editName, executable: this.executable,
      argumentsText: this.argumentsText, workingDirectory: this.workingDirectory,
      d3dBackend: GAME_BACKENDS[this.backendIndex], compatMode: GAME_COMPAT_MODES[this.compatIndex],
      openglRenderer: GAME_OPENGL_RENDERERS[this.openglIndex],
      resolution: GAME_RESOLUTIONS[this.resolutionIndex], environmentText: this.environmentText,
      lastLaunchedAt: this.library.getProfile(game).lastLaunchedAt };
  }
  private save(): boolean {
    const game = this.currentGame();
    if (!game) return false;
    try { this.library.saveProfile(game, this.draft(game)); this.dirty = false; return true; }
    catch (err) { this.message = err instanceof Error ? err.message : '保存失败'; return false; }
  }
  private async runGame(game: GameEntry, allowRestart: boolean = false): Promise<void> {
    const result = await this.library.launch(game, allowRestart);
    this.message = result.message;
    if (result.requiresRestart) {
      AlertDialog.show({ title: '需要更换桌面分辨率', message: result.message + ' 重启会结束当前所有 Windows 程序。',
        primaryButton: { value: '取消', action: () => {} },
        secondaryButton: { value: '已保存，重启并启动', action: () => { this.runGame(game, true); } } });
    }
  }
  private launchCard(game: GameEntry): void {
    if (game.id === this.selectedId && !this.save()) return;
    this.runGame(game);
  }
  private launchSelected(): void {
    const game = this.currentGame();
    if (game && this.save()) this.runGame(game);
  }
  private closeWindow(): void {
    const close = (): void => {
      (getContext(this) as common.UIAbilityContext).terminateSelf();
    };
    if (!this.dirty) { close(); return; }
    AlertDialog.show({ title: '设置尚未保存', message: '关闭游戏库不会停止正在运行的游戏，但未保存的设置会丢失。',
      primaryButton: { value: '继续编辑', action: () => {} },
      secondaryButton: { value: '放弃并关闭', action: close } });
  }
  onBackPress(): boolean { this.closeWindow(); return true; }

  @Builder
  fieldLabel(text: string) {
    Text(text).fontSize(12).fontColor('#AEBCCE').width('100%').margin({ top: 8, bottom: 4 })
  }

  @Builder
  gameCard(game: GameEntry) {
    Row({ space: 10 }) {
      Text(this.library.getProfile(game).name.substring(0, 1).toUpperCase())
        .fontSize(18).fontWeight(FontWeight.Bold).fontColor('#A4C8FF')
        .width(36).height(40).textAlign(TextAlign.Center).backgroundColor('#23354C').borderRadius(9)
      Column({ space: 4 }) {
        Text(this.library.getProfile(game).name).fontSize(14).fontColor('#F3F6FA')
          .width('100%').maxLines(2).textOverflow({ overflow: TextOverflow.Ellipsis })
        Text(game.executables.length ? `${game.executables.length} 个启动程序` : '未找到 .exe，请检查是否已解压')
          .fontSize(11).fontColor('#91A1B7').width('100%').maxLines(1)
      }.layoutWeight(1).alignItems(HorizontalAlign.Start)
      Button(this.launchingId === game.id ? '准备中' : '启动')
        .height(30).fontSize(12).backgroundColor('#2B609E')
        .enabled(game.executables.length > 0 && !this.launchingId)
        .onClick(() => this.launchCard(game))
    }.width('100%').padding(8).borderRadius(10)
      .backgroundColor(this.selectedId === game.id ? '#24364E' : '#182331')
      .border({ width: 1, color: this.selectedId === game.id ? '#4D82C5' : '#243141' })
      .onClick(() => this.chooseGame(game))
  }

  @Builder
  gameDetails(game: GameEntry) {
    Column() {
      Row() {
        Text('游戏设置').fontSize(16).fontWeight(FontWeight.Bold).fontColor('#EEF3FA').layoutWeight(1)
        Text(this.dirty ? '未保存' : '已保存').fontSize(12).fontColor(this.dirty ? '#FFD28A' : '#93A9C2')
        Button('恢复默认').height(28).fontSize(12).margin({ left: 12 }).backgroundColor('#263448')
          .enabled(!this.launchingId)
          .onClick(() => { this.loadProfile(defaultGameProfile(game)); this.dirty = true; })
      }.width('100%').padding({ bottom: 6 })
      Scroll() {
        Column() {
          this.fieldLabel('显示名称')
          TextInput({ text: this.editName, placeholder: game.title }).maxLength(120).height(34).id('game-library-name')
            .fontColor('#F3F6FA').backgroundColor('#111B28')
            .onChange((value: string) => { if (value !== this.editName) { this.editName = value; this.dirty = true; } })
          this.fieldLabel('主程序（递归发现的全部 .exe）')
          if (game.executables.length) {
            Select(game.executables.map((path: string): SelectOption => ({ value: path })))
              .value(this.executable || '请选择主程序').selected(game.executables.indexOf(this.executable))
              .font({ size: 13 }).fontColor('#F3F6FA').backgroundColor('#111B28').width('100%')
              .height(34).id('game-library-program')
              .onSelect((index: number) => {
                if (game.executables[index] !== this.executable) { this.executable = game.executables[index]; this.dirty = true; }
              })
          } else {
            Text('此目录暂无可启动程序。放入解压后的游戏后点击刷新。').fontColor('#D1AB73').fontSize(13)
          }
          Text('安装器、卸载器不会自动作为主程序；同目录的 DLC / 其他入口可在这里选择。')
            .fontSize(11).fontColor('#91A1B7').width('100%').margin({ top: 6 })
          this.fieldLabel('启动参数（支持双引号）')
          TextInput({ text: this.argumentsText, placeholder: '例如：-windowed -lang zh_CN' }).maxLength(4096).height(34)
            .id('game-library-arguments')
            .fontColor('#F3F6FA').backgroundColor('#111B28')
            .onChange((value: string) => { if (value !== this.argumentsText) { this.argumentsText = value; this.dirty = true; } })
          this.fieldLabel('工作目录（相对于 games；留空使用主程序所在目录）')
          TextInput({ text: this.workingDirectory, placeholder: game.directory || '留空使用程序目录' }).maxLength(2048).height(34)
            .id('game-library-working-directory')
            .fontColor('#F3F6FA').backgroundColor('#111B28')
            .onChange((value: string) => { if (value !== this.workingDirectory) { this.workingDirectory = value; this.dirty = true; } })
          this.fieldLabel('图形后端')
          Select(BACKENDS).selected(this.backendIndex).value(BACKENDS[this.backendIndex].value)
            .font({ size: 14 }).fontColor('#F3F6FA').backgroundColor('#111B28').width('100%')
            .height(34).id('game-library-backend')
            .onSelect((index: number) => { if (index !== this.backendIndex) { this.backendIndex = index; this.dirty = true; } })
          this.fieldLabel('OpenGL 渲染器')
          Select(OPENGL).selected(this.openglIndex).value(OPENGL[this.openglIndex].value)
            .font({ size: 14 }).fontColor('#F3F6FA').backgroundColor('#111B28').width('100%')
            .height(34).id('game-library-opengl')
            .onSelect((index: number) => { if (index !== this.openglIndex) { this.openglIndex = index; this.dirty = true; } })
          Text('自动按程序选择；Zink 只在已验证的手机和运行时上启用。').fontSize(11).fontColor('#91A1B7')
          this.fieldLabel('CPU / Box64 兼容性')
          Select(COMPAT).selected(this.compatIndex).value(COMPAT[this.compatIndex].value)
            .id('game-library-cpu')
            .font({ size: 14 }).fontColor('#F3F6FA').backgroundColor('#111B28').width('100%')
            .height(34)
            .onSelect((index: number) => { if (index !== this.compatIndex) { this.compatIndex = index; this.dirty = true; } })
          if (deviceInfo.deviceType === 'phone') {
            this.fieldLabel('手机桌面分辨率')
            Select(RESOLUTIONS).selected(this.resolutionIndex).value(RESOLUTIONS[this.resolutionIndex].value)
              .font({ size: 14 }).fontColor('#F3F6FA').backgroundColor('#111B28').width('100%')
              .height(34).id('game-library-resolution')
              .onSelect((index: number) => { if (index !== this.resolutionIndex) { this.resolutionIndex = index; this.dirty = true; } })
            Text('分辨率属于整个 Wine 会话。与当前会话不同时，启动前会提示保存并重启；不会修改全局偏好。')
              .fontSize(11).fontColor('#91A1B7').width('100%').margin({ top: 6 })
          }
          this.fieldLabel('高级环境变量（每行 KEY=VALUE，可留空）')
          TextArea({ text: this.environmentText, placeholder: '仅对本游戏启动生效；以 # 开头的行是注释' })
            .height(92).maxLength(16384).fontColor('#F3F6FA').backgroundColor('#111B28').fontSize(13)
            .id('game-library-environment')
            .onChange((value: string) => { if (value !== this.environmentText) { this.environmentText = value; this.dirty = true; } })
          Text(`游戏目录：${game.directory || 'games 根目录'}`).fontSize(11).fontColor('#91A1B7')
            .width('100%').margin({ top: 12, bottom: 16 })
        }.width('100%').padding({ right: 8 }).alignItems(HorizontalAlign.Start)
      }.layoutWeight(1).width('100%').scrollBar(BarState.Auto).id('game-library-settings')
      Row({ space: 12 }) {
        Button('保存设置').layoutWeight(1).height(34).backgroundColor('#304359').id('game-library-save')
          .enabled(!this.launchingId).onClick(() => { this.save(); })
        Button(this.launchingId === game.id ? '正在启动…' : '保存并启动').layoutWeight(1).height(34)
          .backgroundColor('#3479CF').enabled(game.executables.length > 0 && !this.launchingId)
          .onClick(() => this.launchSelected())
      }.width('100%').padding({ top: 8 })
    }.padding(12).height('100%').layoutWeight(1).backgroundColor('#172231').borderRadius(12)
  }

  build() {
    Column({ space: 8 }) {
      Row({ space: 10 }) {
        Text('久鱼游戏库').fontSize(21).fontWeight(FontWeight.Bold).fontColor('#F3F6FA')
        Text(this.engineStatus).fontSize(12).fontColor('#91A1B7').layoutWeight(1).maxLines(1)
        Button('进入画面').height(32).fontSize(12).backgroundColor('#304359').enabled(this.desktopAvailable)
          .onClick(() => {
            WineWindowManager.getInstance().showDesktopForCaller(getContext(this) as common.UIAbilityContext)
              .catch((err: Error) => { this.message = '无法打开画面：' + JSON.stringify(err); });
          })
        Button(this.scanning ? '扫描中…' : '刷新 games').height(32).fontSize(12).backgroundColor('#304359')
          .enabled(!this.launchingId).onClick(() => { this.library.refresh(); })
        Button('关闭窗口').height(32).fontSize(12).backgroundColor('#263448').onClick(() => this.closeWindow())
      }.width('100%')
      Text(this.root || '正在获取已授权的 games 目录…').fontSize(11).fontColor('#91A1B7')
        .width('100%').maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
      Row({ space: 14 }) {
        Column({ space: 10 }) {
          Row({ space: 6 }) {
            Button('按游戏目录').layoutWeight(1).height(30).fontSize(12)
              .backgroundColor(this.showPrograms ? '#263448' : '#2B609E')
              .onClick(() => this.setListingMode(false))
            Button('全部程序').layoutWeight(1).height(30).fontSize(12)
              .backgroundColor(this.showPrograms ? '#2B609E' : '#263448')
              .onClick(() => this.setListingMode(true))
          }.width('100%')
          Search({ value: this.query, placeholder: '搜索游戏或启动程序' }).height(32).id('game-library-search')
            .fontColor('#F3F6FA').backgroundColor('#1B2A3D')
            .onChange((value: string) => { this.query = value; })
          List({ space: 9 }) {
            ForEach(this.visibleGames(), (game: GameEntry) => {
              ListItem() { this.gameCard(game) }
            }, (game: GameEntry) => game.id)
          }.layoutWeight(1).width('100%').scrollBar(BarState.Auto).id('game-library-list')
          if (!this.visibleGames().length) {
            Text(this.scanning ? '正在递归发现游戏…' : '暂无匹配游戏\n请将解压后的游戏放入 games，再刷新')
              .fontSize(13).fontColor('#91A1B7').width('100%').textAlign(TextAlign.Center).padding(16)
          }
        }.width('35%').height('100%')
        if (this.currentGame()) {
          this.gameDetails(this.currentGame() as GameEntry)
        } else {
          Column() {
            Text('选择一个游戏查看设置').fontSize(20).fontColor('#D0DCEE')
            Text('设置按游戏独立保存，关闭此窗口不会停止游戏。').fontSize(13).fontColor('#91A1B7').margin({ top: 12 })
          }.layoutWeight(1).height('100%').justifyContent(FlexAlign.Center).backgroundColor('#172231').borderRadius(12)
        }
      }.layoutWeight(1).width('100%')
      Row({ space: 8 }) {
        if (this.scanning || this.launchingId) { LoadingProgress().width(18).height(18).color('#76ABF0') }
        Text(this.message).fontSize(12).fontColor('#C1D0E3').layoutWeight(1).maxLines(2)
      }.width('100%').constraintSize({ minHeight: 20 })
    }.width('100%').height('100%').padding(12).backgroundColor('#101822')
  }
}
