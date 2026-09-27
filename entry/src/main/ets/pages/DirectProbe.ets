import testNapi from 'libentry.so';

class DirectOutputController extends XComponentController {
  onSurfaceCreated(surfaceId: string): void {
    testNapi.setDirectProbeSurfaceId(BigInt(surfaceId));
  }

  onSurfaceDestroyed(surfaceId: string): void {
    testNapi.clearDirectProbeSurfaceId(BigInt(surfaceId));
  }
}

@Entry
@Component
struct DirectProbe {
  private outputController: DirectOutputController = new DirectOutputController();

  build() {
    Column() {
      Text('Native Direct Probe')
        .fontSize(24)
      XComponent({
        id: 'direct_output_xc',
        type: XComponentType.SURFACE,
        controller: this.outputController
      })
        .width(320)
        .height(240)
        .backgroundColor('#111111')
      Text('Probe result is saved in the application cache.')
        .fontSize(16)
    }
    .width('100%')
    .height('100%')
    .justifyContent(FlexAlign.Center)
  }
}
