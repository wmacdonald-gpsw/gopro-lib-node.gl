#!/usr/bin/env python
# -*- coding: utf-8 -*-
#
# Copyright 2018 GoPro Inc.
#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#

import os
import os.path as op
import sys

from PyQt5 import QtCore, QtGui, QtWidgets

from pynodegl_utils.com import query_subproc
from pynodegl_utils.config import Config
from pynodegl_utils.misc import Media
from pynodegl_utils.scriptsmgr import ScriptsManager

from pynodegl_utils.ui.gl_view import GLView
from pynodegl_utils.ui.graph_view import GraphView
from pynodegl_utils.ui.export_view import ExportView
from pynodegl_utils.ui.serial_view import SerialView
from pynodegl_utils.ui.toolbar import Toolbar
from pynodegl_utils.ui.hooks import Hooks


class MainWindow(QtWidgets.QSplitter):

    error = QtCore.pyqtSignal(str)

    def __init__(self, module_pkgname, assets_dir, hooksdir):
        super(MainWindow, self).__init__(QtCore.Qt.Horizontal)
        self._win_title_base = 'Node.gl viewer'
        self.setWindowTitle(self._win_title_base)

        self._module_pkgname = module_pkgname
        self._scripts_mgr = ScriptsManager(module_pkgname)
        self._hooksdir = hooksdir

        medias = None
        if assets_dir:
            medias = []
            for f in sorted(os.listdir(assets_dir)):
                ext = f.rsplit('.', 1)[-1].lower()
                path = op.join(assets_dir, f)
                if op.isfile(path) and ext in ('mp4', 'mkv', 'avi', 'webm', 'mov', 'lrv'):
                    try:
                        media = Media(path)
                    except:
                        pass
                    else:
                        medias.append(media)

        self._medias = medias

        get_scene_func = self._get_scene

        self._hooks = None
        self._config = Config(module_pkgname)

        # Apply previous geometry (position + dimensions)
        rect = self._config.get('geometry')
        if rect:
            geometry = QtCore.QRect(*rect)
            self.setGeometry(geometry)

        gl_view = GLView(get_scene_func, self._config)
        graph_view = GraphView(get_scene_func, self._config)
        export_view = ExportView(get_scene_func, self._config)
        serial_view = SerialView(get_scene_func)

        self._tabs = [
            ('Player view', gl_view),
            ('Graph view', graph_view),
            ('Export', export_view),
            ('Serialization', serial_view),
        ]
        self._last_tab_index = -1

        self._tab_widget = QtWidgets.QTabWidget()
        for tab_name, tab_view in self._tabs:
            self._tab_widget.addTab(tab_view, tab_name)
        self._tab_widget.currentChanged.connect(self._currentTabChanged)

        self._scene_toolbar = Toolbar(self._config)
        self._scene_toolbar.sceneChanged.connect(self._scene_changed)
        self._scene_toolbar.sceneChanged.connect(self._scene_changed_hook)
        self._scene_toolbar.sceneChanged.connect(self._config.scene_changed)
        self._scene_toolbar.aspectRatioChanged.connect(gl_view.set_aspect_ratio)
        self._scene_toolbar.aspectRatioChanged.connect(export_view.set_aspect_ratio)
        self._scene_toolbar.aspectRatioChanged.connect(self._config.set_aspect_ratio)
        self._scene_toolbar.samplesChanged.connect(gl_view.set_samples)
        self._scene_toolbar.samplesChanged.connect(self._config.set_samples)
        self._scene_toolbar.frameRateChanged.connect(gl_view.set_frame_rate)
        self._scene_toolbar.frameRateChanged.connect(graph_view.set_frame_rate)
        self._scene_toolbar.frameRateChanged.connect(export_view.set_frame_rate)
        self._scene_toolbar.frameRateChanged.connect(self._config.set_frame_rate)
        self._scene_toolbar.logLevelChanged.connect(self._config.set_log_level)
        self._scene_toolbar.clearColorChanged.connect(gl_view.set_clear_color)
        self._scene_toolbar.clearColorChanged.connect(self._config.set_clear_color)
        self._scene_toolbar.backendChanged.connect(gl_view.set_backend)
        self._scene_toolbar.backendChanged.connect(self._config.set_backend)

        self._scene_toolbar.hudChanged.connect(self._config.set_hud)

        self._errbuf = QtWidgets.QPlainTextEdit()
        self._errbuf.setFont(QtGui.QFontDatabase.systemFont(QtGui.QFontDatabase.FixedFont))
        self._errbuf.setReadOnly(True)
        self._errbuf.hide()

        self._hooks_lbl = QtWidgets.QLabel()
        self._hooks_layout = QtWidgets.QHBoxLayout()
        self._hooks_layout.addWidget(self._hooks_lbl)
        self._hooks_widget = QtWidgets.QWidget()
        self._hooks_widget.setLayout(self._hooks_layout)
        self._hooks_widget.hide()

        tabs_and_errbuf = QtWidgets.QVBoxLayout()
        tabs_and_errbuf.addWidget(self._tab_widget)
        tabs_and_errbuf.addWidget(self._errbuf)
        tabs_and_errbuf.addWidget(self._hooks_widget)
        tabs_and_errbuf_widget = QtWidgets.QWidget()
        tabs_and_errbuf_widget.setLayout(tabs_and_errbuf)

        self.addWidget(self._scene_toolbar)
        self.addWidget(tabs_and_errbuf_widget)
        self.setStretchFactor(1, 1)

        self._scene_toolbar.reload_btn.clicked.connect(self._scripts_mgr.reload)  # TODO: drop
        self._scripts_mgr.error.connect(self._all_scripts_err)
        self._scripts_mgr.scriptsChanged.connect(self._scene_toolbar.on_scripts_changed)
        self._scripts_mgr.start()

        self.error.connect(self._scene_err)

        # Load the previous scene if the current and previously loaded
        # module packages match
        prev_pkgname = self._config.get('pkg')
        prev_module = self._config.get('module')
        prev_scene = self._config.get('scene')
        if prev_pkgname == module_pkgname:
            self._scene_toolbar.load_scene_from_name(prev_module, prev_scene)

    @QtCore.pyqtSlot(int, int, str)
    def _hooks_uploading(self, i, n, filename):
        self._hooks_widget.show()
        self._hooks_lbl.setText('Uploading [%d/%d]: %s...' % (i, n, filename))

    @QtCore.pyqtSlot(str, str)
    def _hooks_building_scene(self, backend, system):
        self._hooks_widget.show()
        self._hooks_lbl.setText('Building %s scene in %s...' % (system, backend))

    @QtCore.pyqtSlot()
    def _hooks_sending_scene(self):
        self._hooks_widget.show()
        self._hooks_lbl.setText('Sending scene...')

    @QtCore.pyqtSlot()
    def _hooks_done(self):
        self._hooks_widget.hide()

    @QtCore.pyqtSlot(str)
    def _scene_err(self, err_str):
        if err_str:
            self._errbuf.setPlainText(err_str)
            self._errbuf.show()
            sys.stderr.write(err_str)
        else:
            self._errbuf.hide()

    @QtCore.pyqtSlot(str)
    def _all_scripts_err(self, err_str):
        self._scene_toolbar.clear_scripts()
        self._scene_err(err_str)

    def _get_scene(self, **cfg_overrides):
        cfg = self._scene_toolbar.get_cfg()
        if cfg['scene'] is None:
            return None
        cfg['pkg'] = self._module_pkgname
        cfg['medias'] = self._medias
        cfg.update(cfg_overrides)

        self._scripts_mgr.pause()
        ret = query_subproc(query='scene', **cfg)
        if 'error' in ret:
            self._scripts_mgr.resume()
            self.error.emit(ret['error'])
            return None

        self.error.emit(None)
        self._scripts_mgr.set_filelist(ret['filelist'])
        self._scripts_mgr.resume()
        self._scene_toolbar.set_cfg(ret)

        return ret

    @QtCore.pyqtSlot(str, str)
    def _scene_changed(self, module_name, scene_name):
        self.setWindowTitle('%s - %s.%s' % (self._win_title_base, module_name, scene_name))
        self._currentTabChanged(self._tab_widget.currentIndex())

    @QtCore.pyqtSlot(str, str)
    def _scene_changed_hook(self, module_name, scene_name):
        if self._hooks:
            self._hooks.wait()
        self._hooks = Hooks(self._get_scene, self._hooksdir)
        self._hooks.uploadingFileNotif.connect(self._hooks_uploading)
        self._hooks.buildingSceneNotif.connect(self._hooks_building_scene)
        self._hooks.sendingSceneNotif.connect(self._hooks_sending_scene)
        self._hooks.finished.connect(self._hooks_done)
        self._hooks.error.connect(self._hooks_error)
        self._hooks.start()

    @QtCore.pyqtSlot(str)
    def _hooks_error(self, err):
        QtWidgets.QMessageBox.critical(self, 'Hook error', err, QtWidgets.QMessageBox.Ok)

    def _emit_geometry(self):
        geometry = (self.x(), self.y(), self.width(), self.height())
        self._config.geometry_changed(geometry)

    @QtCore.pyqtSlot(QtGui.QResizeEvent)
    def resizeEvent(self, resize_event):
        super(MainWindow, self).resizeEvent(resize_event)
        self._emit_geometry()

    @QtCore.pyqtSlot(QtGui.QMoveEvent)
    def moveEvent(self, move_event):
        super(MainWindow, self).moveEvent(move_event)
        self._emit_geometry()

    @QtCore.pyqtSlot(QtGui.QCloseEvent)
    def closeEvent(self, close_event):
        for name, widget in self._tabs:
            widget.close()
        super(MainWindow, self).closeEvent(close_event)

    @QtCore.pyqtSlot(int)
    def _currentTabChanged(self, index):
        next_view = self._tabs[index][1]
        prev_view = self._tabs[self._last_tab_index][1]
        if index != self._last_tab_index and hasattr(prev_view, 'leave'):
            prev_view.leave()
        if hasattr(next_view, 'enter'):
            next_view.enter()
        self._last_tab_index = index
