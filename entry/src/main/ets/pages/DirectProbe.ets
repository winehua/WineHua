@Entry
@Component
struct DirectProbe {
  build() {
    Column() {
      Text('Native Direct Probe')
        .fontSize(24)
      Text('Probe result is saved in the application cache.')
        .fontSize(16)
    }
    .width('100%')
    .height('100%')
    .justifyContent(FlexAlign.Center)
  }
}
