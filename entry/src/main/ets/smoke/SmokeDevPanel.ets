import { SmokeRunner } from './SmokeRunner';
import { WineEnvService, writeTextFile } from '../service/WineEnvService';
import displayRouteNapi from 'libdisplay_route_napi.so';
import { componentUtils } from '@kit.ArkUI';

// 侧边栏「开发测试」区 (Index 唯一的 smoke 侵入行: <SmokeDevPanel />)。
// 使能与进度全部内部镜像, Index 零状态零逻辑。合并 main-ui 摘除 =
// 删标记行 + 删 ets/smoke/ 整目录。
@Component
export struct SmokeDevPanel {
  @State private smokeStatus: string = '';
  @State private smokeResult: string = '';
  @State private canRun: boolean = false;
  // T8: DisplayRoute 出图链的 XComponent surface (onLoad 后可启动)
  @State private drSurfaceId: string = '';
  private drController: XComponentController = new XComponentController();

  private onRefresh = (): void => {
    const runner = SmokeRunner.getInstance();
    const envSvc = WineEnvService.getInstance();
    this.smokeStatus = runner.statusText();
    this.smokeResult = runner.resultText();
    this.canRun = !envSvc.isBusy() && envSvc.prefixReady && !runner.isRunning();
    if (runner.isRunning()) {
      this.writePreviewRect();
    }
  };

  // D22 §2.4 判据债的设备端数据落盘: 把预览 XComponent 的 on-screen 物理
  // 矩形写成 files 根下的标记文件 (smoke.py 按 DISPLAYROUTE_MARKERS 归档进
  // device-results/)。设备端只产数据, 裁剪与四象限判定都在主机侧 —— 判定
  // 只读归档。运行期间每次刷新都重写 (布局随窗口状态变化的场景留新值);
  // 组件未挂载时静默跳过, 归档缺文件 = 判定器诚实 SKIP。
  private writePreviewRect(): void {
    try {
      const info: componentUtils.ComponentInfo =
        this.getUIContext().getComponentUtils().getRectangleById('displayroute-test');
      const payload = JSON.stringify({
        x: Math.round(info.screenOffset.x), y: Math.round(info.screenOffset.y),
        width: Math.round(info.size.width), height: Math.round(info.size.height),
        surfaceId: this.drSurfaceId,
      });
      writeTextFile(`${getContext(this).filesDir}/displayroute-preview-rect.json`,
        payload);
    } catch (_) {
      // 组件未挂载/未布局: 不写文件, 判定侧按矩形缺失走 SKIP
    }
  }

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
      Button('DisplayRoute 启动')
        .width('100%').height(32).backgroundColor('#748')
        .onClick(() => {
          // surfaceId 已就绪则带出图链启动, 否则仅合成器内核 (T7 行为)
          const sid = this.drSurfaceId !== '' ? BigInt(this.drSurfaceId) : 0n;
          displayRouteNapi.startDisplayRouteChain(sid);
        })
        .margin({ bottom: 6 })
      XComponent({
        id: 'displayroute-test',
        type: XComponentType.SURFACE,
        controller: this.drController
      })
        .width('100%').aspectRatio(4 / 3)
        .backgroundColor('#000')
        .onLoad(() => {
          this.drSurfaceId = this.drController.getXComponentSurfaceId();
          // 全局桥: smoke job 的 displayroute 触发也带出图链 (SmokeRunner 读取)
          AppStorage.setOrCreate('smoke.drSurfaceId', this.drSurfaceId);
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
