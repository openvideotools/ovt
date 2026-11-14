// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The "Sample streams" list of the Welcome page (chapter 11 §11.8, WI-1.464): a handful of small conformance
// streams read from tests/corpora/manifest.json, each with its size and licence so that the consent dialog can show
// both before anything is fetched. This model never downloads; the fetch itself starts from the consent signal.
//
// SPEC-ASSUMPTION(#309): the Welcome page lists at most kMaxEntries streams, the smallest "fast"-tagged entries of
// the manifest that have a URL; §11.8 does not say which manifest entries are samples or where the installed
// manifest lives (OVT_CORPORA_MANIFEST, else <application dir>/../share/ovt/corpora/manifest.json).

#ifndef OVT_APP_MODELS_SAMPLE_STREAMS_H
#define OVT_APP_MODELS_SAMPLE_STREAMS_H

#include <QtQml/qqmlregistration.h>
#include <QObject>
#include <QString>
#include <QVariantList>

#include <cstdint>

namespace ovt::app {

class SampleStreams : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_SINGLETON
  // Each entry: id, url, licence (text), licenceSpdx, sizeBytes, sizeText, codecs (list of names).
  Q_PROPERTY(QVariantList entries READ entries NOTIFY entriesChanged)
  Q_PROPERTY(bool available READ available NOTIFY entriesChanged)

 public:
  static constexpr int kMaxEntries = 5;

  SampleStreams();
  explicit SampleStreams(const QString& manifest_path, QObject* parent = nullptr);

  [[nodiscard]] QVariantList entries() const { return entries_; }
  [[nodiscard]] bool available() const { return !entries_.isEmpty(); }

  // Replaces the list from `manifest_path`; false (list empty) when the file is missing or unusable.
  Q_INVOKABLE bool load(const QString& manifest_path);
  // "1.7 MB", "960 KB": the size shown in the consent dialog.
  Q_INVOKABLE [[nodiscard]] static QString sizeText(double bytes);
  // Where the installed manifest is looked for (see the SPEC-ASSUMPTION above).
  [[nodiscard]] static QString defaultManifestPath();

 Q_SIGNALS:
  void entriesChanged();

 private:
  QVariantList entries_;
};

}  // namespace ovt::app

#endif  // OVT_APP_MODELS_SAMPLE_STREAMS_H
