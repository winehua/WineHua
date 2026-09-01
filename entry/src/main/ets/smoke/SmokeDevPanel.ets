import { SmokeRunner } from './SmokeRunner';
import { WineEnvService } from '../service/WineEnvService';

// 侧边栏「开发测试」区 (Index 唯一的 smoke 侵入行: <SmokeDevPanel />)。
// 使能与进度全部内部镜像, Index 零状态零逻辑。合并 main-ui 摘除 =
// 删标记行 + 删 ets/smoke/ 整目录。
@Component
export struct SmokeDevPanel {
  @State private smokeStatus: string = '';
  @State private smokeResult: string = '';
  @State private canRun: boolean = false;

  private onRefresh = (): void => {
    const runner = SmokeRunner.getInstance();
    const envSvc = WineEnvService.getInstance();
    this.smokeStatus = runner.statusText();
    this.smokeResult = runner.resultText();
    this.canRun = !envSvc.isBusy() && envSvc.prefixReady && !runner.isRunning();
  };

  aboutToAppear(): void {
    SmokeRunner.getInstance().subscribe(this.onRefresh);
    WineEnvService.getInstance().subscribe(this.onRefresh);
    this.onRefresh();
  }

  aboutToDisappear(): void {
    SmokeRunner.getInstance().unsubscribe(this.onRefresh);
    WineEnvService.getInstance().unsubscribe(this.onRefresh);
  }

  build() {
    Column() {
      Text('开发测试')
        .fontWeight(FontWeight.Bold).fontColor('#999')
        .width('100%').padding({ top: 12, bottom: 8 })
      Button('Smoke 回归 (core)')
        .width('100%').height(32).backgroundColor('#394')
        .enabled(this.canRun)
        .onClick(() => {
          SmokeRunner.getInstance().start({
            suite: 'core',
            runId: `manual-${Date.now()}`,
            prefix: 'reuse',
            source: 'ui'
          });
        })
        .margin({ bottom: 6 })
      if (this.smokeStatus) {
        Text(this.smokeStatus)
          .fontSize(12).fontColor(this.smokeResult === 'PASS' ? '#4f4' :
            (this.smokeResult === 'FAIL' || this.smokeResult === 'ERROR' ? '#f55' : '#8cf'))
          .width('100%').maxLines(3)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
          .margin({ bottom: 4 })
        Text('结果: C:\\smoke\\results\\' + (SmokeRunner.getInstance().runId() || '') +
          '\\suite-summary.json')
          .fontSize(11).fontColor('#777')
          .width('100%').maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
      }
    }
    .width('100%')
  }
}
