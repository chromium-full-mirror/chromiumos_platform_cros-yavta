/*
Copyright 2014 cros-yavta authors

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */
var defered = {};
var timer={};

var consts = {
  // v4l2_buf_type
  V4L2_BUF_TYPE_VIDEO_CAPTURE:  1,

  // Control flags
  V4L2_CTRL_FLAG_DISABLED:      0x0001,
  V4L2_CTRL_FLAG_GRABBED:       0x0002,
  V4L2_CTRL_FLAG_READ_ONLY:     0x0004,
  V4L2_CTRL_FLAG_UPDATE:        0x0008,
  V4L2_CTRL_FLAG_INACTIVE:      0x0010,
  V4L2_CTRL_FLAG_SLIDER:        0x0020,
  V4L2_CTRL_FLAG_WRITE_ONLY:    0x0040,
  V4L2_CTRL_FLAG_VOLATILE:      0x0080,

  // v4l2_ctrl_type
  V4L2_CTRL_TYPE_INTEGER:       1,
  V4L2_CTRL_TYPE_BOOLEAN:       2,
  V4L2_CTRL_TYPE_MENU:          3,
  V4L2_CTRL_TYPE_BUTTON:        4,
  V4L2_CTRL_TYPE_INTEGER64:     5,
  V4L2_CTRL_TYPE_CTRL_CLASS:    6,
  V4L2_CTRL_TYPE_STRING:        7,
  V4L2_CTRL_TYPE_BITMASK:       8,
  V4L2_CTRL_TYPE_INTEGER_MENU:  9,

  // device_state
  DEVICE_STATE_STOP: 0,
  DEVICE_STATE_CAPTURE: 1,
};

function open_device(id, cb) {
  $.get('/device/open', {'id':id}, function(result) {
      $('#close'+id).show();
      if (cb) cb();
  });
}

function close_device(id, cb) {
  if (cb) cb();
}

// Show error message on the screen
// Returns true if no error
function handle_error(obj) {
  if (obj['error'] == undefined) {
    return true;
  }
  $('#msg').text(obj['error']);
  $('#msg').effect('highlight', {color:'red'}, 2000);
  return false;
}

function get_current_state(id) {
  // current format and size
  $.get('/device/'+id+'/format', function(result) {
    var format = result.format_name;
    $('#format'+id).text(format)
    var size = result.width + 'x' + result.height;
    $('#size'+id).text(size)
  });

  // current framerate
  $.get('/device/'+id+'/streamparm', function(result) {
    var framerate = result.timeperframe[0] + '/' + result.timeperframe[1];
    $('#framerate'+id).text(framerate)
  });
}

function set_control_value(id, ctrl, ctrlidx, value, cb) {
  var clsidx = id+'_'+ctrl.cls;
  if (!$('#cb_autoapply'+clsidx).is(':checked')) {
    var clsidx = id+'_'+ctrl.cls;
    defered[clsidx][ctrl.id] = {type: ctrl.type, value: value};
    return;
  }

  $.post('/device/'+id+'/control/'+ctrl.id, {
    type: ctrl.type, value: value,
    reserved_0: $('#reserved'+id).val(),
  }, function(result) {
    if (handle_error(result)) {
      if (ctrl.flags & consts.V4L2_CTRL_FLAG_UPDATE) {
        refresh_all_control(id, false, cb);
      } else {
        refresh_one_control(id, ctrl, ctrlidx, cb);
      }
    }
  });
}

function set_control_value_defered(id, ctrlcls) {
  var clsidx = id+'_'+ctrlcls;
  var types = [];
  var values = [];
  var ids = [];
  var df = defered[clsidx];
  for (id in df) {
    ids.push(id);
    types.push(df[id].type);
    values.push(df[id].value);
  }
  defered[clsidx]={};
  $.post('/device/'+id+'/control/'+ids.join(), {
    type: types.join(), value: values.join(),
    reserved_0: $('#reserved'+id).val(),
  }, function(result) {
    handle_error(result);
    refresh_all_control(id, false);
  });
}

function add_one_control(cp, id, ctrl, ctrlidx) {
  var item =
    $('<div>').attr('class', 'ctrlitem').append(
      $('<span>').attr('class', 'ctrlname'),
      $('<span>').attr({id:'ctrlval'+ctrlidx, class:'ctrlval'}),
      $('<div>').attr('class', 'ctrlbox').append(
        $('<span>').attr('class', 'ctrlmin'),
        $('<span>').attr('class', 'ctrlentry'),
        $('<span>').attr('class', 'ctrlmax')));
  item.find('.ctrlname').text(ctrl.name);
  item.find('.ctrlmin').text(ctrl.minimum);
  item.find('.ctrlmax').text(ctrl.maximum);
  var entry = item.find('.ctrlentry');

  var clsidx = id+'_'+ctrl.cls;
  var ctrlgrp = $('#ctrlcls'+clsidx);
  if ($('#ctrlcls'+clsidx).length == 0) {
    cp.append(
	  // TODO class name. driver haven't support it yet
	$('<div>').attr('class', 'clsbox').append(
	  $('<span>').attr('class', 'ctrlclsname').text('class 0x'+ctrl.cls.toString(16)),
	  $('<button>').attr('id', 'btn_toggle_expand'+clsidx).text('hide/show'),
	  $('<br>'),
	  $('<div>').attr({id:'ctrlcls'+clsidx, 'class':'ctrlgroup'}).append(
	    $('<hr>'),
	    $('<input>').attr({'id': 'cb_autoapply'+clsidx,
	      'type':'checkbox', 'checked': true}),
	    $('<label>').text('Auto apply'),
	    $('<button>').attr('id', 'btn_apply'+clsidx).text('Apply').hide(),
	    $('<button>').attr('id', 'btn_clear'+clsidx).text('Clear').hide())));
    ctrlgrp = $('#ctrlcls'+clsidx);
    $('#btn_toggle_expand'+clsidx).click(function(){
      ctrlgrp.toggle();
    });
    $('#btn_apply'+clsidx).click(function() {
      set_control_value_defered(id, ctrl.cls);
    });
    $('#btn_clear'+clsidx).click(function() {
      defered[clsidx] = {}
      refresh_all_control(id, false);
    });
    $('#cb_autoapply'+clsidx).click(function() {
      $('#btn_apply'+clsidx).toggle();
      $('#btn_clear'+clsidx).toggle();
      defered[clsidx] = {}
    });
    defered[clsidx] = {}
  }
  ctrlgrp.find('hr').before(item);

  var ctrlid = 'ctrl'+ctrlidx;
  switch (ctrl.type) {
    case consts.V4L2_CTRL_TYPE_INTEGER:
      entry.append($('<div>').attr('id', ctrlid));
      $('#'+ctrlid).slider({
        value: ctrl.value,
        min: ctrl.minimum,
        max: ctrl.maximum,
        step: ctrl.step,
        slide: function(event, ui) {
          $('#ctrlval'+ctrlidx).text(ui.value);
          set_control_value(id, ctrl, ctrlidx, ui.value);
        },
      });
      break;
    case consts.V4L2_CTRL_TYPE_BOOLEAN:
      entry.append(
          $('<input>').attr({id:ctrlid, type:'checkbox'}),
          $('<label>').attr('for', ctrlid).text('Toggle'));
      $('#'+ctrlid).button().click(function() {
        var value = $(this).is(':checked')?1:0;
        set_control_value(id, ctrl, ctrlidx, value);
      });
      break;
    case consts.V4L2_CTRL_TYPE_BUTTON:
      entry.append($('<button>').attr('id', ctrlid).text('click'));
      $('#'+ctrlid).button().click(function(){
        set_control_value(id, ctrl, ctrlidx, 1);
      });
      break;
    case consts.V4L2_CTRL_TYPE_CTRL_CLASS:
      entry.append($('<div>').attr('class', 'ctrlclassname').text(ctrl.name));
      break;
    case consts.V4L2_CTRL_TYPE_MENU:
    case consts.V4L2_CTRL_TYPE_INTEGER_MENU:
      entry.append($('<select>').attr('id', ctrlid));
      if (ctrl.menu.length == 0) {
        $('#'+ctrlid).append($('<option>').text('(no valid item)'));
      } else {
        $.each(ctrl.menu, function (loop_idx, item) {
          var display_name;
          if (ctrl.type == consts.V4L2_CTRL_TYPE_MENU) {
            display_name = item.index+': '+item.name;
          } else {
            display_name = item.value;
          }
          $('#'+ctrlid).append($('<option>').attr('value', item.index).text(display_name));
        });
        $('#'+ctrlid).change(function() {
          var value = $('#'+ctrlid).val();
          set_control_value(id, ctrl, ctrlidx, value);
          return false;
        });
      }
      break;
    case consts.V4L2_CTRL_TYPE_BITMASK:
      item.find('.ctrlmin').hide();
      item.find('.ctrlmax').hide();
      var set_callback = function() {
	var value = $('#'+ctrlid).val();
	if (value.match(/^0x/)) {
	  value = parseInt(value.substr(2), 16);
	} else if (value.match(/^0b/)) {
	  value = parseInt(value.substr(2), 2);
	}
	set_control_value(id, ctrl, ctrlidx, value);
      }
      entry.append(
	$('<input>').attr({'id': ctrlid, 'type':'text'}).change(set_callback),
	$('<button>').button().text('set').click(set_callback));
      break;

    case consts.V4L2_CTRL_TYPE_INTEGER64:
    case consts.V4L2_CTRL_TYPE_STRING:
      entry.append('TODO type '+ctrl.type+', not supported yet');
  }
}

function update_one_control(ctrl, ctrlidx, value) {
  if (value == undefined) {
    $('#ctrlval'+ctrlidx).text('undefined');
    return;
  }
  $('#ctrlval'+ctrlidx).text(value);
  var node = $('#ctrl'+ctrlidx);
  switch (ctrl.type) {
    case consts.V4L2_CTRL_TYPE_INTEGER:
      node.slider('value', value);
      if (ctrl.flags & (consts.V4L2_CTRL_FLAG_GRABBED | consts.V4L2_CTRL_FLAG_READ_ONLY)) {
        node.slider('disable');
      } else {
        node.slider('enable');
      }
      break;
    case consts.V4L2_CTRL_TYPE_BOOLEAN:
      if (parseInt(value)) {
        node.attr('checked', 'checked').button('refresh');
        node.button('widget').text('Enabled');
      } else {
        node.removeAttr('checked').button('refresh');
        node.button('widget').text('Disabled');
      }
      break;
    case consts.V4L2_CTRL_TYPE_BUTTON:
      // do nothing
      break;
    case consts.V4L2_CTRL_TYPE_MENU:
    case consts.V4L2_CTRL_TYPE_INTEGER_MENU:
      node.find('[value="'+value+'"]').attr('selected', true);
      break;
    case consts.V4L2_CTRL_TYPE_BITMASK:
      node.val('0b'+parseInt(value).toString(2));
      $('#ctrlval'+ctrlidx).text(node.val());
      break;
  }
}

function refresh_all_control(id, add, cb) {
  var cp = $('#control_panel'+id);
  $.get('/device/'+id+'/control', {
    reserved_0: $('#reserved'+id).val(),
  }, function(result) {
    $.each(result, function (loop_idx, ctrl) {
      var ctrlidx = id + '_' + loop_idx;
      if (add) {
        add_one_control(cp, id, ctrl, ctrlidx);
      }
      update_one_control(ctrl, ctrlidx, ctrl.value);
    });
    if (cb) cb();
  });
}

function refresh_one_control(id, ctrl, ctrlidx, cb) {
  $.get('/device/'+id+'/control/'+ctrl.id, {
    reserved_0: $('#reserved'+id).val(),
  }, function(result) {
    if (handle_error(result)) {
      update_one_control(ctrl, ctrlidx, result);
    } else {
      update_one_control(ctrl, ctrlidx, undefined);
    }
    if (cb) cb();
  });
}

function setup_control_panel(id) {
  var cp = $('#control_panel'+id);

  cp.append(
      $('<label>').text('temporal layer:'),
      $('<select>').attr('id', 'reserved'+id).append(
        $('<option>').text(0),
        $('<option>').text(1),
        $('<option>').text(2),
        $('<option>').text(3),
        $('<option>').text(7).attr('selected', true)),
      $('<br>'));

  $('#reserved'+id).change(function() {
    refresh_all_control(id, false);
  });

  cp.append(
      $('<button>')
      .attr('id', 'btn_control_default'+id)
      .text('Set all default')
      .click(function() {
        $.post('/device/'+id+'/reset_control', {
          reserved_0: $('#reserved'+id).val(),
        }, function() {
          refresh_all_control(id, false);
        });
      }));
  cp.append(
      $('<button>')
      .attr('id', 'btn_control_query'+id)
      .text('Query current')
      .click(function() {
        refresh_all_control(id, false);
      }));

  cp.append(
      $('<input>').attr({id: 'cb_polling'+id, type:'checkbox'}),
      $('<label>').text('Polling value'));
  $('#cb_polling'+id).click(function(){
    if ($(this).is(':checked')) {
      timer[id] = setInterval(function(){
        if ($('#cb_polling'+id).is(':checked')) {
          refresh_all_control(id, false);
        } else {
          clearInterval(timer[id]);
          timer[id] = undefined;
        }
      }, 1000);
    } else {
      clearInterval(timer[id]);
      timer[id] = undefined;
    }
  });

  cp.append($('<hr>'));


  refresh_all_control(id, true, function() {
    cp.show();
  });
}

function setup_format_dialog(id) {
  $('#dialog'+id).html(
      $('<form>').append(
        $('<fieldset>').append(
          $('<label>').attr('for', 'sel_format'+id).text('Format'),
          $('<select>').attr('id', 'sel_format'+id),
          $('<label>').attr('for', 'sel_size'+id).text('Size'),
          $('<select>').attr('id', 'sel_size'+id),
          $('<label>').attr('for', 'sel_framerate'+id).text('Framerate'),
          $('<select>').attr('id', 'sel_framerate'+id))));

  $('#dialog'+id).dialog({
    autoOpen: false,
    modal: true,
    buttons: {
      'Set': function() {
        var format = $('#sel_format'+id).val();
        var size = $('#sel_size'+id).val().split(',');
        $.post('/device/'+id+'/format', {
          format: format, width: size[0], height: size[1],
        }, function (result) {
          handle_error(result);
          $('#dialog'+id).dialog('close');
        });
      },
      'Cancel': function() {
        $(this).dialog('close');
      }
    },
    close: function() {
    },
  });

  // fetch format when dialog opened
  $('#dialog'+id).dialog('option', 'open', function() {
    $.get('/device/'+id+'/formats', {
      type: consts.V4L2_BUF_TYPE_VIDEO_CAPTURE
    }, function(result) {
      var orig_format = $('#sel_format'+id).val();
      $('#sel_format'+id).html('');
      $.each(result, function (loop_idx, item) {
        $('#sel_format'+id).append(
          $('<option>').attr('value', item.format).text(item.format_name));
      });
      if (orig_format) {
        $('#sel_format'+id).val(orig_format);
      }
      $('#sel_format'+id).change();
    });
  });

  // if format changed, fetch values of frame sizes
  $('#sel_format'+id).change(function() {
    var format_value = $(this).val();
    $.get('/device/'+id+'/frame_sizes', {
      format: format_value,
    }, function(result) {
      var orig_size = $('#sel_size'+id).val();
      $('#sel_size'+id).html('');
      $.each(result, function (loop_idx, item) {
	// TODO: support non-discrete size
        var text = item.min[0] + 'x' + item.min[1];
        var v = item.min[0] + ','+ item.min[1];
        $('#sel_size'+id).append(
          $('<option>').attr('value', v).text(text));
      });
      if (orig_size) {
        $('#sel_size'+id).val(orig_size);
      }
      $('#sel_size'+id).change();
    });
  });

  // if frame size changed, fetch values of frame rates
  $('#sel_size'+id).change(function() {
    var format_value = $('#sel_format'+id).val();
    var size_value = $(this).val().split(',');
    $.get('/device/'+id+'/frame_intervals', {
      format: format_value,
      width: size_value[0],
      height: size_value[1],
    }, function(result) {
      var orig_rate = $('#sel_framerate'+id).val();
      $('#sel_framerate'+id).html('');
      $.each(result, function (loop_idx, item) {
	// TODO: support non-discrete rate
        var a = item.min[0];
        var b = item.min[1];
        var value = a+','+b;
        var text = (b/a) + ' fps';
        $('#sel_framerate'+id).append(
          $('<option>').attr('value', value).text(text));
      });
      if (orig_rate) {
        $('#sel_framerate'+id).val(orig_rate);
      }
    });
  });

  $('#btn_change'+id).click(function() {
    $('#dialog'+id).dialog('open');
  });
}

function add_device(id, devname) {
  $('#root').append(('<div id=device% class=device>'+
        '<div class=devname>'+devname+'</div>'+
        '<button id=btn_show% class="inactive%">Show detail</button>'+
        '<button id=btn_video% class="active%">Video</button>'+
        '<button id=btn_hide% class="active%">Hide detail</button>'+
        '<button id=btn_close% class="active%">Close</button>'+
        '<div id=info% class="panel active%">'+
        '  <span>format:</span><span id=format%></span>'+
        '  <span>framerate:</span><span id=framerate%></span>'+
        '  <span>size:</span><span id=size%></span>'+
        '</div>'+
        '<button id=btn_capture_on% class="active%">Stream On</button>'+
        '<button id=btn_capture_off% class="active%">Stream Off</button>'+
        '<div id=dialog% title="Change format">'+
        '</div>'+
        '<div id=video%/>'+
        '<button id=btn_change% class="active%">Change...</button>'+
        '<button id=btn_control% class="active%">Control...</button>'+
        '<div id=control_panel% class="control_panel"></div>'+
        '</div>').replace(/%/g, id));
  $('.active'+id).hide();

  $('#control_panel'+id).hide();

  // ------------------------------------
  // show, hide, close
  $('#btn_show'+id).click(function() {
    $('.inactive'+id).hide();
    $('#close'+id).show();
    $('.active'+id).show();
    get_current_state(id);
  });

  $('#btn_hide'+id).click(function() {
    $('.active'+id).hide();
    $('.inactive'+id).show();
  });

  $('#btn_close'+id).click(function() {
    // TODO
  });

  $('#btn_video'+id).click(function() {
    $('#video'+id).html(
      $('<video>').attr({'autoplay': true}).append(
        $('<source>').attr(
          {'src': '/webm/'+id, 'type': 'video/webm'})));
  });

  $('#btn_capture_on'+id).button().click(function() {
    $.post('/device/'+id+'/capture', {
      on: 1,
    }, handle_error);
  });
  $('#btn_capture_off'+id).button().click(function() {
    $.post('/device/'+id+'/capture', {
      on: 0,
    }, handle_error);
  });
  // ------------------------------------
  // change format
  setup_format_dialog(id);

  // ------------------------------------
  // control
  $('#btn_control'+id).click(function() {
    if ($('#control_panel'+id).is(':visible')) {
      $('#control_panel'+id).html('');
      $('#control_panel'+id).hide();
    } else {
      setup_control_panel(id);
    }
  });
}

function init() {
  // set global ajax default
  $.ajaxSetup({cache:false});
  $(document).ajaxError(function(e, xhr, settings, exception) {
    $('#msg').text('exception: ' + exception + ', response:\n' + xhr.responseText );
  });

  $.get('/device', function(data){
    var devices = data;
    $.each(devices, function(i, dev) {
      add_device(dev.id, dev.devname);
    });
  });

  $('#open').click(function() {
    $.get('/device/list', function(devlist) {
      var dev_dialog = $('<form id=dlg_open>').append($('<select id=openname>'));
      dev_dialog.dialog({
        autoOpen: true,
        modal: true,
        title: 'Open device',
        buttons: {
          'Open': function() {
            var devname = $('#openname').val();
            $.post('/device/open', {
              devname: devname
            }, function(result) {
              if (handle_error(result)) {
                add_device(result, devname);
              }
              $('#dlg_open').dialog('close');
            });
          },
          'Cancel': function() {
            $('#dlg_open').dialog('close');
          }
        },
        close: function() {
          $('#dlg_open').remove();
        },
      });

      $.each(devlist, function(idx, devname) {
        $('#openname').append($('<option>').text(devname));
      });
    });
  });
}

$(document).ready(init);
// vim:sw=2:expandtab
