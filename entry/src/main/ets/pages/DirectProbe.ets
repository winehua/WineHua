import testNapi from 'libentry.so';

class DirectOutputController extends XComponentController {
  surfaceId: string = '0';

  onSurfaceCreated(surfaceId: string): void {
    this.surfaceId = surfaceId;
    testNapi.setDirectProbeSurfaceId(BigInt(surfaceId));
  }

  onSurfaceChanged(surfaceId: string, rect: SurfaceRect): void {
    testNapi.setDirectProbeSurfaceSize(BigInt(surfaceId), rect.surfaceWidth, rect.surfaceHeight);
  }

  onSurfaceDestroyed(surfaceId: string): void {
    testNapi.clearDirectProbeSurfaceId(BigInt(surfaceId));
    if (this.surfaceId === surfaceId) this.surfaceId = '0';
  }
}

@Entry
@Component
struct DirectProbe {
  private outputController: DirectOutputController = new DirectOutputController();
  @StorageLink('winehua.directProbe.resize') resizeMode: boolean = false;
  @State private outputWidth: number = 320;
  @State private outputHeight: number = 240;
  private resizeTimer: number = -1;
  private resized: boolean = false;

  aboutToAppear(): void {
    if (this.resizeMode) this.pollResizeRequest();
  }

  aboutToDisappear(): void {
    if (this.resizeTimer >= 0) clearTimeout(this.resizeTimer);
    this.resizeTimer = -1;
  }

  private pollResizeRequest(): void {
    if (this.resized || !this.resizeMode) return;
    if (testNapi.takeDirectProbeResizeRequest(BigInt(this.outputController.surfaceId)) as boolean) {
      this.resized = true;
      this.outputWidth = 400;
      this.outputHeight = 160;
      return;
    }
    this.resizeTimer = setTimeout(() => this.pollResizeRequest(), 30);
  }

  build() {
    Column() {
      Text('Native Direct Probe')
        .fontSize(24)
      XComponent({
        id: 'direct_output_xc',
        type: XComponentType.SURFACE,
        controller: this.outputController
      })
        .width(this.outputWidth)
        .height(this.outputHeight)
        .backgroundColor('#111111')
      Text('Probe result is saved in the application cache.')
        .fontSize(16)
    }
    .width('100%')
    .height('100%')
    .justifyContent(FlexAlign.Center)
  }
}
