"""Maintainer-only PDF fixtures; requires pypdf (not an app dependency).

All artwork/fixtures in this script are original Vulkana MIT test material.
Never uses the owner's private reference PDFs.
"""
from pathlib import Path
from pypdf import PdfWriter
from pypdf.generic import (ArrayObject, DecodedStreamObject, DictionaryObject,
                          FloatObject, NameObject, NumberObject)

DESTINATION = Path(__file__).parent / 'fixtures/pdf'
DESTINATION.mkdir(parents=True, exist_ok=True)


def stream(writer, content, extra=None):
    item = DecodedStreamObject()
    item.set_data(content.encode('ascii'))
    if extra:
        item.update(extra)
    return writer._add_object(item)


def rect(values):
    return ArrayObject([FloatObject(v) for v in values])


def save(name, writer):
    writer.add_metadata({'/Producer': 'Vulkana original test fixture'})
    with (DESTINATION / name).open('wb') as output:
        writer.write(output)


writer = PdfWriter()
for size in [(120, 80), (80, 160), (72.36, 36.12), (64, 64)]:
    page = writer.add_blank_page(*size)
    if len(writer.pages) != 4:
        page[NameObject('/Contents')] = stream(writer,
            '1 0 0 rg 0 0 20 20 re f 0 0 1 rg 30 30 20 20 re f\n')
writer._root_object[NameObject('/PageLabels')] = DictionaryObject({NameObject('/Nums'): ArrayObject([
    NumberObject(0), DictionaryObject({NameObject('/S'): NameObject('/r')}),
    NumberObject(2), DictionaryObject({NameObject('/S'): NameObject('/D'), NameObject('/St'): NumberObject(1)})])})
save('pages.pdf', writer)

writer = PdfWriter()
page = writer.add_blank_page(160, 120)
page[NameObject('/CropBox')] = rect([20, 30, 140, 100])
# Rotation is inherited from the page-tree node, not the page dictionary.
writer._pages.get_object()[NameObject('/Rotate')] = NumberObject(90)
page[NameObject('/Contents')] = stream(writer,
    '1 0 0 rg 20 30 20 20 re f 0 0 1 rg 120 80 20 20 re f\n')
save('rotated-crop.pdf', writer)

writer = PdfWriter()
page = writer.add_blank_page(80, 60)
page[NameObject('/UserUnit')] = NumberObject(2)
page[NameObject('/Contents')] = stream(writer, '0 1 0 rg 0 0 40 30 re f\n')
save('user-unit.pdf', writer)

writer = PdfWriter()
page = writer.add_blank_page(100, 100)
page[NameObject('/Resources')] = DictionaryObject({NameObject('/ExtGState'): DictionaryObject({
    NameObject('/Half'): DictionaryObject({NameObject('/Type'): NameObject('/ExtGState'),
                                         NameObject('/ca'): FloatObject(0.5)})})})
page[NameObject('/Contents')] = stream(writer,
    '1 1 1 rg 0 0 20 20 re f q /Half gs 1 0 0 rg 30 30 30 30 re f Q\n')
save('transparency.pdf', writer)

writer = PdfWriter()
page = writer.add_blank_page(100, 100)
appearance = stream(writer, '0 0 1 rg 0 0 20 20 re f\n', {
    NameObject('/Type'): NameObject('/XObject'), NameObject('/Subtype'): NameObject('/Form'),
    NameObject('/BBox'): rect([0, 0, 20, 20])})
annotation = DictionaryObject({NameObject('/Type'): NameObject('/Annot'), NameObject('/Subtype'): NameObject('/Square'),
    NameObject('/Rect'): rect([20, 20, 40, 40]), NameObject('/F'): NumberObject(4),
    NameObject('/AP'): DictionaryObject({NameObject('/N'): appearance})})
page[NameObject('/Annots')] = ArrayObject([writer._add_object(annotation)])
save('annotations.pdf', writer)
writer.encrypt('test-password', 'test-owner', algorithm='AES-256')
save('password.pdf', writer)

writer = PdfWriter()
page = writer.add_blank_page(100, 100)
appearance = stream(writer, '0 1 0 rg 0 0 20 20 re f\n', {
    NameObject('/Type'): NameObject('/XObject'), NameObject('/Subtype'): NameObject('/Form'),
    NameObject('/BBox'): rect([0, 0, 20, 20])})
widget = writer._add_object(DictionaryObject({NameObject('/Type'): NameObject('/Annot'),
    NameObject('/Subtype'): NameObject('/Widget'), NameObject('/FT'): NameObject('/Tx'),
    NameObject('/Rect'): rect([20, 20, 40, 40]), NameObject('/F'): NumberObject(4),
    NameObject('/AP'): DictionaryObject({NameObject('/N'): appearance})}))
page[NameObject('/Annots')] = ArrayObject([widget])
writer._root_object[NameObject('/AcroForm')] = DictionaryObject({NameObject('/Fields'): ArrayObject([widget])})
save('form-appearance.pdf', writer)

writer = PdfWriter()
writer.add_blank_page(100000, 100000)
save('oversized.pdf', writer)
(DESTINATION / 'malformed.pdf').write_bytes(b'%PDF-1.7\nnot a PDF object tree\n%%EOF\n')
